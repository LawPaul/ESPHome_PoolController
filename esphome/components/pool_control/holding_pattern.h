#pragma once

#include <cstdint>

namespace esphome {
namespace pool_control {

// Holding-pattern suction rotation. The idle base circulation runs mostly on
// the skimmer, but periodically pulls from the main drain to turn over the deep
// end and mix top-to-bottom (so freshly generated chlorine doesn't stratify).
//
// Pure + framework-free so it is exercised by host unit tests. The drain window
// sits at the start of each period; everything else is skimmer.
//
//   |<-- drain_s -->|<--------- skimmer --------->|<-- drain_s -->| ...
//   0             drain_s                       period_s
//
// sec_of_day is 0..86399 (negative values are wrapped defensively). Returns
// true when suction should be on the main drain, false for the skimmer.
inline bool holding_drain_now(int sec_of_day, int period_s, int drain_s) {
  if (period_s <= 0 || drain_s <= 0)
    return false;
  if (drain_s > period_s)
    drain_s = period_s;
  int phase = ((sec_of_day % period_s) + period_s) % period_s;
  return phase < drain_s;
}

// Holding SPEED setpoint for the suction path currently selected.
//
// Speed (rpm) mode is open-loop: nothing trims rpm to hold a flow rate, so the
// flow the pump actually makes depends on the head of the path it is pulling
// through — and the two holding suction paths are measurably different. Bench
// reading on this system, both at 20 GPM:
//
//     skimmer      1481 rpm   202 W
//     main drain   1791 rpm   311 W     (+21% rpm, +54% W for the same flow)
//
// So one shared rpm would give ~17% LESS flow on the drain leg than on the
// skimmer leg. The binding constraint (the salt cell's flow switch) applies to
// the worst case, so a single setpoint must either be sized for the drain —
// wasting power for the many hours spent on the skimmer — or risk dropping the
// cell out every time the rotation swings to the drain. Two setpoints avoid
// both. Resolve at COMMAND time, not at scene-apply time: the rotation moves
// valve2 on its own 60 s schedule, long after the scene was applied.
inline float holding_rpm_for_suction(float rpm_skimmer, float rpm_drain,
                                     bool on_drain) {
  return on_drain ? rpm_drain : rpm_skimmer;
}

}  // namespace pool_control
}  // namespace esphome
