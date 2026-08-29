#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace esphome {
namespace pool_control {

// Pure filter-load estimate. A loading filter needs MORE rpm to hold the same
// flow, so current rpm as a % of the clean-baseline rpm is a loading proxy —
// but only meaningful at the reference flow (fixed hydraulics). Framework-free
// so the validity gating + ratio are host-tested; the YAML sensor supplies live
// telemetry and separately gates on being in the fixed-hydraulics mode.
//
// Returns NaN when a valid reading can't be formed: no baseline captured,
// missing telemetry, or flow off the reference point (> 10%).
inline float filter_load_pct(float rpm, float flow, float base_rpm,
                             float base_flow) {
  if (base_rpm <= 0.0f || base_flow <= 0.0f || std::isnan(rpm) ||
      std::isnan(flow))
    return std::numeric_limits<float>::quiet_NaN();
  if (std::fabs(flow - base_flow) > 0.10f * base_flow)
    return std::numeric_limits<float>::quiet_NaN();
  return 100.0f * rpm / base_rpm;
}

// Rescale the raw rpm-ratio load onto an operator-facing "how dirty" axis:
// 0% = as clean as the baseline, 100% = wants backwashing now, >100% = overdue.
//
// The raw ratio is physically honest but reads like a fill level, so a clean
// filter showing 100% looks alarming, and "clean at 120%" is arbitrary to
// anyone who has not read this file. Rescaling puts the decision in the number
// itself. clean_threshold_pct is that trip point, still expressed as % of
// baseline rpm, and it becomes the full-scale end of the new axis. It must be
// > 100: needing LESS rpm than when clean is not a dirtiness condition, and a
// threshold at or below the baseline would make the scale meaningless.
//
// Negative results clamp to 0 -- rpm a hair under baseline is measurement noise
// or slightly cooler water, not a cleaner-than-clean filter. The top is left
// unclamped so an overdue filter keeps climbing past 100% instead of pegging.
inline float filter_dirt_pct(float load_pct, float clean_threshold_pct) {
  if (std::isnan(load_pct) || std::isnan(clean_threshold_pct) ||
      clean_threshold_pct <= 100.0f)
    return std::numeric_limits<float>::quiet_NaN();
  const float dirt =
      100.0f * (load_pct - 100.0f) / (clean_threshold_pct - 100.0f);
  return dirt < 0.0f ? 0.0f : dirt;
}

// True once load has risen above the clean-filter threshold %. A NaN load (no
// valid reading) is never a clean-needed condition. Fed the rescaled dirtiness
// against a threshold of 100, which is the same comparison as raw load against
// the raw threshold -- the rescale is monotonic.
inline bool filter_clean_needed(float load_pct, float threshold_pct) {
  if (std::isnan(load_pct))
    return false;
  return load_pct > threshold_pct;
}

// The rpm-needed-to-hold-a-flow relationship is a property of the WHOLE
// hydraulic path, not of the filter alone — every diverter position changes the
// system curve. Measured on this system, holding 20 GPM takes ~1489 rpm on the
// skimmer leg but ~1791 rpm on the main drain leg: a ~20% swing from valve
// position alone, which would swamp the filter-loading signal we are trying to
// read (and false-trip a 120% threshold). So both baseline capture and load
// reporting are restricted to one reference combination: suction from the
// SKIMMERS, return through the POOL JETS. That is all four diverters
// de-energised, and it is the state the idle holding pattern spends most of its
// time in.
inline bool filter_reference_plumbing(bool spa_suction, bool main_drain_suction,
                                      bool spa_return, bool fountain_return) {
  return !spa_suction && !main_drain_suction && !spa_return && !fountain_return;
}

// Consecutive-stable-sample counter, used to wait out valve travel, pump ramps
// and the post-service settle before a reading is trusted. Returns the updated
// count: one higher while rpm stays within tol_rpm of the previous sample, and
// 0 on any excursion or unusable reading (which restarts the settle window).
inline uint16_t filter_steady_count(float rpm, float prev_rpm, uint16_t count,
                                    float tol_rpm) {
  if (std::isnan(rpm) || std::isnan(prev_rpm) || rpm <= 0.0f || prev_rpm <= 0.0f)
    return 0;
  if (std::fabs(rpm - prev_rpm) > tol_rpm)
    return 0;
  return count == 0xFFFFu ? count : static_cast<uint16_t>(count + 1);
}

// Whether an armed baseline capture may fire now. Deliberately says nothing
// about HOW the capture was requested: the user arms it from Service Mode with
// the filter open, and the reading is taken later, once the equipment is back
// in the reference state and has held still. pool_idle is already false while
// service/freeze/cleaning own the equipment, so it covers those too.
//
// `loop_settled` is the holding loop's own verdict on whether what the pump is
// doing right now is a steady operating point rather than a transient on the
// way to one (see HoldingFlowController::settled_for_baseline). Stillness alone
// cannot tell the difference: the loop's establishment plateau is perfectly
// steady for minutes and is at the wrong flow.
inline bool filter_capture_ready(bool armed, bool pump_running, bool pool_idle,
                                 bool reference_plumbing, bool loop_settled,
                                 float rpm, float flow, uint16_t steady_count,
                                 uint16_t required_count) {
  if (!armed || !pump_running || !pool_idle || !reference_plumbing ||
      !loop_settled)
    return false;
  if (std::isnan(rpm) || std::isnan(flow) || rpm <= 0.0f || flow <= 0.0f)
    return false;
  return steady_count >= required_count;
}

}  // namespace pool_control
}  // namespace esphome
