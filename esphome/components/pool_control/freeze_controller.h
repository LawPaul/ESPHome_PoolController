#pragma once

#include <cmath>

namespace esphome {
namespace pool_control {

// Freeze-protection decision for one tick. Pure + framework-free so the
// hysteresis and the loop-rotation phase math are exercised by host unit
// tests; the YAML lambda only marshals sensor reads and actuates relays.
//
// Rotation: three phases, each `rotate_s` long, cycle by second-of-day so no
// exposed plumbing sits static:
//   phase 0 -> pool suction / skimmer     (valve1=Pool,  valve2=Skimmer)
//   phase 1 -> pool suction / main drain  (valve1=Pool,  valve2=Drain)
//   phase 2 -> spa loop                   (valve1=Spa,   valve3=Spa)
// valve1 and valve3 always move together (never a spillover state, which would
// ice the spillway); fountains (valve4) are always held off.
struct FreezeDecision {
  bool active{false};    // desired freeze state after this tick
  bool entered{false};   // transitioned inactive -> active this tick
  bool exited{false};    // transitioned active -> inactive this tick
  float forced_flow{0};  // pump flow (GPM) to command on entry
  int phase{0};          // 0/1/2 rotation phase (for logging)
  bool valve1_spa{false};   // suction: spa (else pool)
  bool valve2_drain{false}; // pool-loop sub-select: main drain (else skimmer); apply only when !valve1_spa
  bool valve3_spa{false};   // return: spa (else pool); mirrors valve1_spa
};

// Caller handles the service-mode override and NaN temperature before calling
// (both mean "do nothing this tick"). `hysteresis_c` is the release margin: once
// active, freeze holds until temp > threshold + hysteresis. `min_runtime_ms` is
// a minimum run-on: once active, freeze ALSO holds until at least that long has
// elapsed since entry, regardless of temperature (mirrors Pentair IntelliCenter's
// "Freeze Cycle Time" — a duration floor that absorbs chatter around the setpoint
// so the threshold can sit right at the trigger with only a small release margin).
// `active_elapsed_ms` is how long freeze has been active this spell (0 when
// inactive); ignored on entry. Both default to 0 = no run-on floor.
inline FreezeDecision freeze_tick(float temp_c, float threshold_c, float hysteresis_c,
                                  bool was_active, int sec_of_day, int rotate_s,
                                  float normal_flow,
                                  uint32_t active_elapsed_ms = 0,
                                  uint32_t min_runtime_ms = 0) {
  FreezeDecision d;

  if (temp_c < threshold_c) {
    d.active = true;
  } else if (was_active && temp_c > threshold_c + hysteresis_c &&
             active_elapsed_ms >= min_runtime_ms) {
    d.active = false;
  } else {
    d.active = was_active;  // hold: inside hysteresis band OR within min run-on
  }
  d.entered = d.active && !was_active;
  d.exited = !d.active && was_active;

  // Vigorous circulation on entry: at least 48 GPM, or the normal flow if higher.
  float f = normal_flow;
  if (f < 48.0f)
    f = 48.0f;
  d.forced_flow = f;

  int phase = (rotate_s > 0) ? (((sec_of_day % (3 * rotate_s)) + 3 * rotate_s) % (3 * rotate_s)) / rotate_s : 0;
  d.phase = phase;
  bool spa = (phase == 2);
  d.valve1_spa = spa;
  d.valve3_spa = spa;
  d.valve2_drain = (phase == 1);  // only meaningful on the pool loop (!spa)
  return d;
}

// True when a periodically-updated temperature source has gone stale.
// `last_seen_ms` must be advanced ONLY on a valid (non-NaN) reading, so a probe
// returning NaN — or a never-configured probe (last_seen_ms stays 0) — trips
// after max_stale_ms. Lets freeze protection fail SAFE on a dead sensor.
inline bool temp_sensor_stale(uint32_t now_ms, uint32_t last_seen_ms,
                              uint32_t max_stale_ms) {
  return (now_ms - last_seen_ms) >= max_stale_ms;
}

// Fail-safe freeze decision for when the temperature is unreadable/stale: we
// cannot prove the plumbing is warm, so protect. Feeds a temperature far below
// any threshold into the normal tick, so entry, forced flow and loop rotation
// are identical to a real freeze — only the trigger differs.
inline FreezeDecision freeze_failsafe(float threshold_c, float hysteresis_c,
                                      bool was_active, int sec_of_day,
                                      int rotate_s, float normal_flow) {
  return freeze_tick(-1000.0f, threshold_c, hysteresis_c, was_active,
                     sec_of_day, rotate_s, normal_flow);
}

// Just the plumbing targets for an ACTIVE freeze (rotation valves + forced
// flow), decoupled from the hysteresis/enter/exit state machine. The plumbing
// arbiter uses this to produce the freeze pattern without re-running the
// temperature decision. Forcing an impossibly-cold temp guarantees active, so
// only the rotation (a pure function of sec_of_day / rotate_s) and the forced
// flow are meaningful in the returned decision.
inline FreezeDecision freeze_plumbing(int sec_of_day, int rotate_s,
                                      float normal_flow) {
  return freeze_tick(-1000.0f, 0.0f, 1.0f, true, sec_of_day, rotate_s,
                     normal_flow);
}

}  // namespace pool_control
}  // namespace esphome
