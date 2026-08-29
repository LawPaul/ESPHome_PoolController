// Host unit tests for the always-on supervisor tick.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_supervisor.cpp -o /tmp/svtest && /tmp/svtest
#include "../components/pool_control/supervisor.h"

#include <cassert>
#include <cstdio>

using esphome::pool_control::supervise;
using esphome::pool_control::SupervisorInputs;
using esphome::pool_control::SupervisorOutputs;

// A baseline "everything healthy, pump running".
static SupervisorInputs base() {
  SupervisorInputs in{};
  in.pump_running = true;
  in.pump_priming = false;
  in.pump_alarm = false;
  in.service = false;
  in.freeze = false;
  in.cleaning = false;
  in.boost = false;
  in.comms_lost = false;
  in.alarm = false;
  in.warn = false;
  return in;
}

int main() {
  // Healthy baseline: booster permitted, LED green (healthy).
  {
    auto o = supervise(base());
    assert(o.booster_permit);
    assert(o.status_led == esphome::pool_control::STATUS_HEALTHY);
  }

  // *** KEY SEMANTIC: the booster guard is SERVICE-INDEPENDENT. ***
  // Service mode changes the LED to "service" but must NOT defeat the booster
  // guard while the pump runs.
  {
    auto in = base();
    in.service = true;
    auto o = supervise(in);
    assert(o.booster_permit);  // booster still permitted in service (pump runs)
    assert(o.status_led == esphome::pool_control::STATUS_SERVICE);  // annunciation reacts
  }

  // Freeze mode likewise does not defeat the booster guard.
  {
    auto in = base();
    in.freeze = true;
    auto o = supervise(in);
    assert(o.booster_permit);
    assert(o.status_led == esphome::pool_control::STATUS_FREEZE);
  }

  // Booster guard: pump stopped => booster never permitted, whatever the mode.
  {
    auto in = base();
    in.pump_running = false;
    assert(!supervise(in).booster_permit);
  }
  {
    auto in = base();
    in.pump_running = false;
    in.service = true;
    assert(!supervise(in).booster_permit);  // service can't force a dry booster
  }

  // Booster guard: priming is NOT genuine running (running() stays true during
  // spin-up, before flow) -> booster stays blocked until flow is established.
  {
    auto in = base();
    in.pump_priming = true;  // pump_running still true, but priming
    assert(!supervise(in).booster_permit);
  }

  // Booster guard: a pump fault blocks the booster even while it reports running.
  {
    auto in = base();
    in.pump_alarm = true;
    assert(!supervise(in).booster_permit);
  }

  // LED precedence flows through the aggregate: comms outranks alarm/freeze.
  {
    auto in = base();
    in.comms_lost = true;
    in.alarm = true;
    in.freeze = true;
    assert(supervise(in).status_led == esphome::pool_control::STATUS_COMMS);
  }
  // Service still outranks comms at the LED.
  {
    auto in = base();
    in.service = true;
    in.comms_lost = true;
    assert(supervise(in).status_led == esphome::pool_control::STATUS_SERVICE);
  }

  std::printf("supervisor: all tests passed\n");
  return 0;
}
