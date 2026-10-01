// Host unit tests for the pure HoldingFlowController state machine.
// Build & run:  c++ -std=c++17 -Wall -Wextra esphome/tests/test_holding_flow.cpp
//               -o /tmp/hftest && /tmp/hftest
#include "../components/pool_control/holding_flow.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::HoldingFlowController;
using P = HoldingFlowController::Phase;
using M = HoldingFlowController::Mode;

static bool approx(float a, float b, float eps = 1e-3f) {
  return std::fabs(a - b) < eps;
}

// Convenience: a controller already handed over to the rpm loop at `rpm`,
// which is the state most tests care about. Mirrors the real path -- Off ->
// Establish -> (flow proven + drive settled) -> Hold.
static HoldingFlowController settled_at(float rpm, uint32_t &t) {
  HoldingFlowController c;
  c.tick(t, true, true, 15.5f, 20.0f, 0.0f, true, 0);  // -> Establish
  t += 1000;
  auto o = c.tick(t, true, true, 15.5f, 20.0f, rpm, false, 20);
  assert(o.phase == P::Hold);
  return c;
}

int main() {
  // --- Standing down --------------------------------------------------
  // Cleaning / spa / freeze / service own the pump: never actuate.
  {
    HoldingFlowController c;
    auto o = c.tick(1000, false, true, 15.5f, 15.5f, 1300, false, 20);
    assert(o.phase == P::Off);
    assert(!o.actuate);
  }
  // Pump stopped.
  {
    HoldingFlowController c;
    auto o = c.tick(1000, true, false, 15.5f, 15.5f, 0, true, 0);
    assert(o.phase == P::Off);
    assert(!o.actuate);
  }
  // At or above the drive's floor, flow mode is better than anything we do.
  {
    HoldingFlowController c;
    auto o = c.tick(1000, true, true, 20.0f, 20.0f, 1450, false, 20);
    assert(o.phase == P::Off);
    assert(!o.actuate);
    auto o2 = c.tick(2000, true, true, NAN, 20.0f, 1450, false, 20);
    assert(o2.phase == P::Off);
  }

  // --- Establish ------------------------------------------------------
  // Entry commands FLOW mode at 20 GPM -- guaranteed above the 13.8 reclose.
  {
    HoldingFlowController c;
    auto o = c.tick(1000, true, true, 15.5f, 0.0f, 0, true, 0);
    assert(o.phase == P::Establish);
    assert(o.mode == M::Flow);
    assert(approx(o.setpoint, 20.0f));
    assert(o.actuate);
  }
  // Holds there while the switch is still open.
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    c.tick(t, true, true, 15.5f, 0.0f, 0, true, 0);
    auto o = c.tick(t + 5000, true, true, 15.5f, 5.0f, 900, true, 20);
    assert(o.phase == P::Establish);
    assert(!o.actuate);
  }
  // Flow proven but drive still hunting -> not yet.
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    c.tick(t, true, true, 15.5f, 0.0f, 0, true, 0);
    auto o = c.tick(t + 5000, true, true, 15.5f, 20.0f, 1450, false, 3);
    assert(o.phase == P::Establish);
  }
  // Proven AND settled -> hand over at the drive's OWN rpm. This is the
  // self-calibrating part: nothing is persisted or hardcoded.
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    c.tick(t, true, true, 15.5f, 0.0f, 0, true, 0);
    auto o = c.tick(t + 5000, true, true, 15.5f, 20.0f, 1487, false, 20);
    assert(o.phase == P::Hold);
    assert(o.mode == M::Speed);
    assert(approx(o.setpoint, 1487));
    assert(o.actuate);
  }
  // Never proves flow -> fault, parked in flow mode.
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    c.tick(t, true, true, 15.5f, 0.0f, 0, true, 0);
    auto o = c.tick(t + c.establish_ms, true, true, 15.5f, 1.0f, 900, true, 20);
    assert(o.phase == P::Fault);
    assert(o.mode == M::Flow);
    assert(approx(o.setpoint, 20.0f));
    assert(o.actuate);
    assert(c.faulted());
  }
  // Regression (observed after the 2026-07-26 OTA): the caller ticks every 30s
  // and zeroes the steady counter on the tick that enters Establish, so the
  // count is still one sample short at the first opportunity. The window must
  // be wide enough that missing the first chance is not fatal -- with the old
  // 60s it faulted here with nothing wrong.
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    auto o = c.tick(t, true, true, 15.0f, 20.0f, 1465, false, 0);
    assert(o.phase == P::Establish);
    t += 30000;
    o = c.tick(t, true, true, 15.0f, 20.0f, 1465, false, 14);  // one short
    assert(o.phase == P::Establish);
    assert(!o.actuate);
    t += 30000;
    o = c.tick(t, true, true, 15.0f, 20.0f, 1465, false, 29);
    assert(o.phase == P::Hold);
    assert(approx(o.setpoint, 1465));
  }

  // --- Descent --------------------------------------------------------
  // Walks DOWN in clamped steps. The proportional rescale wants 1124 rpm in
  // one jump (1450*15.5/20); max_step_rpm must contain that, because K rises
  // steeply as rpm falls and the rescale overshoots badly.
  {
    uint32_t t = 1000;
    auto c = settled_at(1450, t);
    t += 1000;
    auto o = c.tick(t, true, true, 15.5f, 20.0f, 1450, false, 20);
    assert(o.phase == P::Hold);
    assert(approx(o.setpoint, 1400));  // clamped, not 1124
    assert(o.actuate);
    t += 1000;
    o = c.tick(t, true, true, 15.5f, 18.6f, 1400, false, 20);
    assert(approx(o.setpoint, 1350));
    t += 1000;
    o = c.tick(t, true, true, 15.5f, 17.2f, 1350, false, 20);
    assert(approx(o.setpoint, 1300));
  }
  // Inside the deadband it does nothing at all -- the steady state.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    auto o = c.tick(t, true, true, 15.5f, 15.2f, 1300, false, 20);
    assert(o.phase == P::Hold);
    assert(approx(o.setpoint, 1300));
    assert(!o.actuate);
  }
  // A loading filter eventually pushes flow out of the band -> rpm rises.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    auto o = c.tick(t, true, true, 15.5f, 14.3f, 1300, false, 20);
    assert(o.actuate);
    assert(o.setpoint > 1300);
  }
  // Not yet steady after our own last change: acting would average across it.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    auto o = c.tick(t, true, true, 15.5f, 10.0f, 1300, false, 4);
    assert(!o.actuate);
    assert(approx(o.setpoint, 1300));
  }
  // The exact situation observed on the pool at 18:40 on 2026-07-26: holding
  // 1317 rpm, averaged flow 15.98, target 15.0. The old rpm/flow rescale wanted
  // -81 rpm (clamped to -50, overshooting to ~13.5 GPM); with the measured gain
  // it is a single -29 rpm move onto target.
  {
    uint32_t t = 1000;
    auto c = settled_at(1317, t);
    t += 1000;
    auto o = c.tick(t, true, true, 15.0f, 15.98f, 1317, false, 20);
    assert(o.actuate);
    assert(approx(o.setpoint, 1317.0f - 30.0f * 0.98f, 0.05f));
    // Landing there reads 15 on a whole-GPM sensor -> error 0 -> it stops.
    t += 1000;
    o = c.tick(t, true, true, 15.0f, 15.0f, o.setpoint, false, 20);
    assert(!o.actuate);
  }

  // --- no_flow recovery ------------------------------------------------
  // A flicker shorter than the debounce is ignored (the switch chattered at
  // the boundary during the sweep).
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    auto o = c.tick(t, true, true, 15.5f, 15.4f, 1300, true, 20);
    assert(o.phase == P::Hold);
    t += 1000;
    o = c.tick(t, true, true, 15.5f, 15.4f, 1300, false, 20);
    assert(o.phase == P::Hold);
  }
  // Sustained -> +10% rpm, immediately.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
    t += 6000;
    auto o = c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
    assert(o.phase == P::Bump);
    assert(o.mode == M::Speed);
    assert(approx(o.setpoint, 1430));
    assert(o.actuate);
    assert(c.trips() == 1);
    // Clears -> straight back to trimming.
    t += 1000;
    o = c.tick(t, true, true, 15.5f, 16.0f, 1430, false, 20);
    assert(o.phase == P::Hold);
  }
  // Doesn't clear -> escalate to a full establish rather than bumping on.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
    t += 6000;
    auto o = c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
    assert(o.phase == P::Bump);
    t += 16000;
    o = c.tick(t, true, true, 15.5f, 2.0f, 1430, true, 20);
    assert(o.phase == P::Establish);
    assert(o.mode == M::Flow);
    assert(o.actuate);
  }
  // Three trips in the window -> stop trying. A repeated trip is far more
  // likely a clogged basket or a failed diverter than "slightly too slow".
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    for (int i = 0; i < 2; i++) {
      t += 1000;
      c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
      t += 6000;
      auto o = c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
      assert(o.phase == P::Bump);
      t += 1000;
      o = c.tick(t, true, true, 15.5f, 16.0f, 1430, false, 20);
      assert(o.phase == P::Hold);
    }
    assert(c.trips() == 2);
    t += 1000;
    c.tick(t, true, true, 15.5f, 2.0f, 1430, true, 20);
    t += 6000;
    auto o = c.tick(t, true, true, 15.5f, 2.0f, 1430, true, 20);
    assert(o.phase == P::Fault);
    assert(c.trips() == 3);
    // Sticky, and does not keep re-actuating once parked.
    t += 60000;
    o = c.tick(t, true, true, 15.5f, 20.0f, 1450, false, 20);
    assert(o.phase == P::Fault);
    assert(!o.actuate);
    // Manual reset re-establishes cleanly.
    c.reset_fault();
    t += 1000;
    o = c.tick(t, true, true, 15.5f, 20.0f, 1450, false, 20);
    assert(o.phase == P::Establish);
    assert(c.trips() == 0);
  }
  // Expire an old trip bucket during normal ticks.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
    t += 6000;
    auto o = c.tick(t, true, true, 15.5f, 2.0f, 1300, true, 20);
    assert(o.phase == P::Bump);
    assert(c.trips() == 1);
    t += 1000;
    c.tick(t, true, true, 15.5f, 16.0f, 1430, false, 20);
    t += c.trip_window_ms;
    c.tick(t, true, true, 15.5f, 16.0f, 1430, false, 20);
    assert(c.trips() == 0);
  }

  // --- Handing the pump back ------------------------------------------
  // Cleaning starts mid-hold: stand down, then re-establish on return rather
  // than resuming a learned rpm against hydraulics that may have changed.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    t += 1000;
    auto o = c.tick(t, false, true, 15.5f, 15.5f, 1300, false, 20);
    assert(o.phase == P::Off);
    assert(!o.actuate);
    t += 1000;
    o = c.tick(t, true, true, 15.5f, 20.0f, 1450, false, 20);
    assert(o.phase == P::Establish);
  }

  // --- Clean-filter baseline gate -------------------------------------
  // The capture needs to know whether the pump is at a real operating point.
  // It must keep working in PLAIN FLOW mode, where we never take over at all.
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    // Target at or above the drive's floor: we stand down permanently, the
    // drive holds the flow itself, and baselining is exactly as it always was.
    auto o = c.tick(t, true, true, 20.0f, 20.0f, 1450, false, 20);
    assert(o.phase == P::Off);
    assert(c.settled_for_baseline());
    // Same when something else owns the pump -- pool_idle already blocks the
    // capture there, but the two must not disagree.
    t += 1000;
    c.tick(t, false, true, 15.0f, 20.0f, 1450, false, 20);
    assert(c.settled_for_baseline());
  }
  // Sub-20 target: NOT while establishing, even though rpm is dead steady and
  // has been for the whole plateau. This is the case that silently poisoned the
  // baseline at 20 GPM.
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    auto o = c.tick(t, true, true, 15.0f, 20.0f, 1465, false, 0);
    assert(o.phase == P::Establish);
    assert(!c.settled_for_baseline());
    t += 30000;
    c.tick(t, true, true, 15.0f, 20.0f, 1465, false, 14);
    assert(!c.settled_for_baseline());  // still establishing
    t += 30000;
    o = c.tick(t, true, true, 15.0f, 20.0f, 1465, false, 29);
    assert(o.phase == P::Hold);
    assert(c.settled_for_baseline());
  }
  // Not while recovering from a trip, and not once we have given up.
  {
    uint32_t t = 1000;
    auto c = settled_at(1300, t);
    assert(c.settled_for_baseline());
    t += 1000;
    c.tick(t, true, true, 15.0f, 15.0f, 1300, true, 20);
    t += 6000;
    auto o = c.tick(t, true, true, 15.0f, 0.0f, 1300, true, 20);
    assert(o.phase == P::Bump);
    assert(!c.settled_for_baseline());
  }
  {
    HoldingFlowController c;
    uint32_t t = 1000;
    c.tick(t, true, true, 15.0f, 0.0f, 0, true, 0);
    auto o = c.tick(t + c.establish_ms, true, true, 15.0f, 1.0f, 900, true, 20);
    assert(o.phase == P::Fault);
    assert(!c.settled_for_baseline());
  }

  printf("test_holding_flow: all assertions passed.\n");
  return 0;
}
