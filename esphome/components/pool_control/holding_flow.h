#pragma once

#include <cmath>
#include <cstdint>

#include "flow_learn.h"  // loop_step()

namespace esphome {
namespace pool_control {

// Closed-loop holding flow BELOW the drive's 20 GPM floor.
//
// WHY. The IntelliFlo3 refuses commanded GPM under 20 in flow mode, and pump
// power goes as roughly the cube of speed, so the whole energy case for the
// idle holding scene lives below that floor. Running there means rpm mode,
// which is open-loop: as the sand filter loads, the same rpm makes less flow,
// and nothing notices. This state machine puts the flow target back in charge
// by trimming rpm, so "hold 15.5 GPM" stays true for weeks.
//
// MEASURED ON THIS POOL (sweep 2026-07-26, idle-skimmer, data/flow_sweep_*.csv):
//
//   reclose : 1250 rpm -> 13.8 GPM   <- the number that matters
//   open    :  800 rpm ->  1.0 GPM
//
// The salt cell's flow switch OPENS at ~1 GPM but does not RECLOSE until
// ~13.8. That 14:1 asymmetry is mechanical stiction, not hysteresis (the pump
// sits below water level, so loss of prime is ruled out). Two consequences
// drive this design:
//
//   1. no_flow is a "flow has collapsed" ALARM, not a "you are near the edge"
//      hint. It cannot be used to trim the target -- it stays closed all the
//      way down and would never warn us. So it is handled as an emergency
//      with bounded escalation, never as a control input.
//   2. Steady running at 15 GPM is stable, but every RE-ESTABLISHMENT after a
//      pump stop (cleaning, freeze, service, reboot, power blip) has to clear
//      ~13.8 GPM or the cell silently stops generating while the flow estimate
//      still reads fine. Hence ESTABLISH below.
//
// ESTABLISH: rather than a hardcoded starting rpm, we command 20 GPM in FLOW
// mode and let the drive's own closed loop settle. It finds the rpm that path
// needs under TODAY's conditions -- filter loading included -- and we take
// over from that rpm. So the handover point self-calibrates on every entry,
// nothing has to be persisted, and it cannot go stale after a filter clean.
//
// NO RPM FLOOR. An rpm floor would be the wrong abstraction: the reclose
// threshold is a property of FLOW (~13.8 GPM), and 1250 was merely the rpm
// that produced it at that moment's hydraulics. With a clean filter the same
// flow needs fewer rpm, and a floor would then force flow above target and
// quietly defeat the loop. Limits live in flow; rpm is left the pump's range.
//
// DEADBAND is 0.5 GPM: half the flow estimate's 1 GPM quantisation. The pump
// reports whole GPM, so a reading of 16 only says the truth is somewhere in
// [15.5, 16.5) -- any error below half a quantum is indistinguishable from
// zero, and acting on it means acting on a number that was never measured.
// Averaging does not rescue this: 44 of 45 consecutive samples read exactly 16,
// so there is no dither to extract sub-LSB information from. The deadband is
// also what prevents a permanent limit cycle on a non-integer target, where the
// reading can only ever straddle the setpoint.
class HoldingFlowController {
 public:
  enum class Phase : uint8_t {
    Off = 0,        // not our business: not idle, pump off, or target >= floor
    Establish = 1,  // flow mode at establish_flow, proving the switch closed
    Hold = 2,       // rpm loop holding target_flow
    Bump = 3,       // no_flow tripped; nudged rpm up, watching for it to clear
    Fault = 4,      // gave up; parked in flow mode, needs a manual reset
  };
  enum class Mode : uint8_t { Flow = 0, Speed = 1 };

  struct Out {
    Phase phase;
    Mode mode;
    float setpoint;  // GPM when mode == Flow, RPM when mode == Speed
    bool actuate;    // true only when the caller should send a command
  };

  // --- Tunables ---
  float establish_flow = 20.0f;  // GPM. The drive's own floor, well above reclose.
  float gain_rpm_per_gpm = 30.0f;  // measured slope is ~31 (see flow_learn.h);
                                   // sitting just under it converges without
                                   // ringing
  float deadband_gpm = 0.5f;     // half the flow estimate's 1 GPM quantisation
  float max_step_rpm = 50.0f;    // bounds one bad average; with a correct gain
                                 // this only binds during establishment
  float rpm_min = 450.0f;        // the pump's range, not a flow guard
  float rpm_max = 3450.0f;
  float bump_frac = 0.10f;             // first recovery rung: +10% rpm
  uint32_t noflow_debounce_ms = 5000;  // the switch flickered at the boundary
  uint32_t bump_ms = 15000;            // then escalate to a full establish
  // Then give up. This must span MANY caller ticks, not two. The handover
  // condition needs `required_steady` rpm samples, and the tick that enters
  // Establish is itself a command, so the caller resets the counter -- meaning
  // the count only just reaches the threshold by the following tick. At 60s
  // that put the pass/fail decision on the same tick as the timeout, and a
  // cold boot (slower first telemetry) lost the race and faulted with nothing
  // actually wrong: observed 2026-07-26, the loop parked at 20 GPM after an OTA
  // yet re-established immediately when stood down and retried. Establishment
  // runs at 20 GPM, i.e. the old fixed behaviour, so waiting longer costs
  // nothing; only a genuine inability to prove flow should reach the fault.
  uint32_t establish_ms = 300000;
  uint16_t required_steady = 15;       // ~30s of samples at one rpm
  uint8_t max_trips = 3;               // per trip_window_ms, then stop trying
  uint32_t trip_window_ms = 86400000u;  // 24h

  // One tick. `flow_avg` must be an AVERAGE over samples taken at a single
  // rpm; `steady_count` (from filter_steady_count) is what guarantees the
  // window does not straddle a change. Returns what to command, if anything.
  Out tick(uint32_t now_ms, bool pool_idle, bool pump_running, float target_flow,
           float flow_avg, float rpm_now, bool no_flow, uint16_t steady_count) {
    // --- Not our business ----------------------------------------------
    // Cleaning, spa, freeze and service own the pump; and at or above the
    // drive's floor, flow mode is strictly better than anything we can do, so
    // we stand down and let the existing holding logic run.
    if (!pool_idle || !pump_running || std::isnan(target_flow) ||
        target_flow >= establish_flow) {
      if (phase_ != Phase::Off) {
        phase_ = Phase::Off;
        noflow_since_ = 0;
        cmd_rpm_ = 0.0f;
      }
      return Out{Phase::Off, Mode::Flow, target_flow, false};
    }

    // A fault is sticky: parked in flow mode until something clears it.
    if (phase_ == Phase::Fault)
      return Out{Phase::Fault, Mode::Flow, establish_flow, false};

    if (phase_ == Phase::Off)
      return enter_establish_(now_ms);

    // --- no_flow debounce (shared by Hold and Bump) ----------------------
    bool tripped = false;
    if (no_flow) {
      if (noflow_since_ == 0)
        noflow_since_ = now_ms ? now_ms : 1u;
      tripped = (now_ms - noflow_since_) >= noflow_debounce_ms;
    } else {
      noflow_since_ = 0;
    }

    switch (phase_) {
      case Phase::Establish: {
        // Wait for proven flow AND a settled drive. steady_count is computed
        // from rpm, so it also tells us the drive has stopped hunting for the
        // rpm that makes establish_flow -- which is the number we take over.
        if (!no_flow && steady_count >= required_steady) {
          cmd_rpm_ = rpm_now;  // hand over from the drive's own answer
          phase_ = Phase::Hold;
          phase_since_ = now_ms;
          // Do not actuate yet: we are already at this rpm, just in flow mode.
          // The next tick starts trimming it down toward target.
          return Out{Phase::Hold, Mode::Speed, cmd_rpm_, true};
        }
        if ((now_ms - phase_since_) >= establish_ms)
          return enter_fault_(now_ms);
        return Out{Phase::Establish, Mode::Flow, establish_flow, false};
      }

      case Phase::Hold: {
        if (tripped)
          return enter_bump_(now_ms);
        // Asserted but still inside the debounce: hold rpm still. Trimming now
        // would act on a collapsed flow reading and then be superseded by the
        // bump a moment later, double-correcting for one event.
        if (no_flow)
          return Out{Phase::Hold, Mode::Speed, cmd_rpm_, false};
        // Still settling after our own last change: the average would straddle
        // it and we would act on a flow that never existed.
        if (steady_count < required_steady)
          return Out{Phase::Hold, Mode::Speed, cmd_rpm_, false};

        float next = loop_step(cmd_rpm_, flow_avg, target_flow, gain_rpm_per_gpm,
                               deadband_gpm, max_step_rpm, rpm_min, rpm_max);
        bool changed = (next != cmd_rpm_);
        cmd_rpm_ = next;
        return Out{Phase::Hold, Mode::Speed, cmd_rpm_, changed};
      }

      case Phase::Bump: {
        if (!no_flow) {  // cleared -- resume trimming
          phase_ = Phase::Hold;
          phase_since_ = now_ms;
          return Out{Phase::Hold, Mode::Speed, cmd_rpm_, false};
        }
        // A trip means flow collapsed to near zero. The usual causes -- a
        // clogged basket, an air lock, a failed diverter -- do not respond to
        // more rpm at all, so this rung is short and then we escalate rather
        // than bumping forever.
        if ((now_ms - phase_since_) >= bump_ms) {
          if (trips_ >= max_trips)
            return enter_fault_(now_ms);
          return enter_establish_(now_ms);
        }
        return Out{Phase::Bump, Mode::Speed, cmd_rpm_, false};
      }

      default:
        return Out{phase_, Mode::Flow, establish_flow, false};
    }
  }

  Phase phase() const { return phase_; }
  float commanded_rpm() const { return cmd_rpm_; }
  uint8_t trips() const { return trips_; }
  bool faulted() const { return phase_ == Phase::Fault; }

  // Whether the current (rpm, flow) pair is a trustworthy clean-filter
  // baseline. Deliberately true in BOTH working modes:
  //
  //   Off  -- we are standing down, so the drive is running its own closed
  //           loop in flow mode (target >= 20, or the fixed-rpm escape hatch).
  //           That is steady by construction and is what the baseline capture
  //           always used to read.
  //   Hold -- our loop owns the rpm. Combined with the caller's stillness
  //           requirement this means converged: Hold cannot act twice inside
  //           60s (it must re-earn `required_steady` after every change), so
  //           rpm holding still for longer than that is the loop declining to
  //           move, i.e. sitting in the deadband.
  //
  // Establish is the one that matters. It is a LONG steady plateau at
  // establish_flow -- the pump ramps, then holds ~90s before the first trim --
  // which looks exactly like a settled system to a stillness detector but is
  // 20 GPM, not the target. Baselining there is silently unrecoverable:
  // filter_load_pct rejects flow more than 10% off the baseline, so a 20 GPM
  // baseline against 15 GPM holding leaves Filter Load permanently unknown.
  // Bump has just kicked rpm +10%; Fault never proved flow at all.
  bool settled_for_baseline() const {
    return phase_ == Phase::Off || phase_ == Phase::Hold;
  }

  // Manual reset: drop back to Off so the next tick re-establishes cleanly.
  void reset_fault() {
    phase_ = Phase::Off;
    trips_ = 0;
    trip_window_start_ = 0;
    noflow_since_ = 0;
    cmd_rpm_ = 0.0f;
  }

 protected:
  Out enter_establish_(uint32_t now_ms) {
    phase_ = Phase::Establish;
    phase_since_ = now_ms;
    noflow_since_ = 0;
    return Out{Phase::Establish, Mode::Flow, establish_flow, true};
  }

  Out enter_bump_(uint32_t now_ms) {
    // Roll the 24h trip window before counting into it.
    if (trip_window_start_ == 0 ||
        (now_ms - trip_window_start_) >= trip_window_ms) {
      trip_window_start_ = now_ms ? now_ms : 1u;
      trips_ = 0;
    }
    if (trips_ < 255)
      trips_++;

    // max_trips is the count at which we STOP trying, so the Nth trip faults
    // rather than bumping once more.
    if (trips_ >= max_trips)
      return enter_fault_(now_ms);

    float bumped = cmd_rpm_ * (1.0f + bump_frac);
    if (bumped > rpm_max) bumped = rpm_max;
    if (bumped < rpm_min) bumped = rpm_min;
    cmd_rpm_ = bumped;
    phase_ = Phase::Bump;
    phase_since_ = now_ms;
    return Out{Phase::Bump, Mode::Speed, cmd_rpm_, true};
  }

  Out enter_fault_(uint32_t now_ms) {
    phase_ = Phase::Fault;
    phase_since_ = now_ms;
    // Park somewhere known good rather than wherever we happened to be.
    return Out{Phase::Fault, Mode::Flow, establish_flow, true};
  }

  Phase phase_{Phase::Off};
  uint32_t phase_since_{0};
  uint32_t noflow_since_{0};
  uint32_t trip_window_start_{0};
  float cmd_rpm_{0.0f};
  uint8_t trips_{0};
};

}  // namespace pool_control
}  // namespace esphome
