#pragma once

#include <cmath>
#include <cstdint>
#include <limits>

namespace esphome {
namespace pool_control {

// Closed-loop flow holding for RPM mode.
//
// WHY THIS EXISTS. Flow mode is already closed-loop inside the pump drive: we
// command GPM and the drive picks the rpm, so a loading filter shows up as
// RISING RPM and the flow the salt cell sees never sags. That is strictly the
// better control mode -- except the drive refuses setpoints below 20 GPM, and
// the whole energy case for this pool lives below 20 (pump power goes as the
// CUBE of speed, so 14 GPM costs roughly a third of what 20 GPM costs).
//
// So: run rpm mode to get under the floor, and rebuild in software the two
// guarantees rpm mode gives up -- holding the target flow as the filter loads,
// and producing a usable filter-health signal.
//
// THE PLANT. Over the narrow band around one target flow, rpm and flow are
// close to proportional, so a single "specific resistance"
//
//     K = rpm / GPM
//
// characterises the hydraulic path. Measured on this system at 20 GPM:
// ~74.5 on the skimmer leg, ~89.6 on the main drain leg.
//
// K IS A LOCAL LINEARISATION, NOT A LAW. It holds where head is dominated by
// pipe friction. It does NOT extrapolate across flows on paths with STATIC
// head -- spillover has to lift water over the weir, so its flow collapses
// non-linearly as rpm falls. That is tolerable here only because each combo is
// run at ONE target flow, so we are always sitting at the point K was learned
// at. Never reuse a learned K at a different target.
//
// Because the plant is near-linear, correction is a rescale rather than a PID:
//
//     rpm_new = rpm_now * (target_flow / measured_flow)
//
// which lands in one or two steps instead of hunting. Everything else here is
// there to make that safe against a noisy flow ESTIMATE (the pump derives GPM
// from a motor model; it is not a flow meter, and it quantises at ~1 GPM).
// The defence is bandwidth, not filtering: filter clogging evolves over WEEKS,
// so updates run on long averages and per-sample noise averages away.
//
// GEOMETRY vs DIRT. Both plumbing changes and a loading filter raise K, so a
// single learned K per combo would confuse them -- and a combo that has not
// run in weeks would come back stale. Split it:
//
//     K_c(t) = G_c * F(t)
//
//   G_c  per-combo GEOMETRY. Fixed plumbing; only changes if a valve fails.
//   F(t) global FILTER LOADING, >= 1. Shared by every combo, because the sand
//        is in every path.
//
// Learned by observing K at steady state: on the reference combo F absorbs the
// change; on every other combo G_c does. A combo learned months ago therefore
// stays correct, since F carries the accumulated dirt forward. Filter load %
// falls out as 100 * F -- which, unlike filter_load_pct(), reads on ANY combo.
//
// STATUS: only loop_step() and learn_ready() are wired. The G_c / F learning,
// the 16-combo table and filter_load_pct_from_factor() are DORMANT, and were
// left in place rather than deleted because they are tested and are the
// obvious extension if suction combos ever need holding too.
//
// They turned out to be unnecessary for the actual problem. Because the loop
// holds FLOW constant, the existing filter_health.h baseline keeps reading
// continuously -- F and filter_load_pct are literally the same quantity when
// flow is pinned:
//
//     F = K_now / K_clean = (rpm_now/flow) / (rpm_clean/flow) = rpm_now/rpm_clean
//
// so the whole learning layer collapses into a ratio we already compute. All it
// costs is pressing Mark Filter Clean once after the loop settles, to move the
// baseline to the holding flow.
//
// SAFETY. The flow ESTIMATE is advisory. The salt cell's flow switch is a real
// mechanical measurement and is the authority: if it opens, the caller raises
// rpm immediately and latches a fault rather than waiting for this loop.

// Four diverters -> 16 possible plumbing states.
static constexpr uint8_t NUM_PLUMBING_KEYS = 16;

// Pack diverter states into a stable table index. Bit order is fixed forever:
// persisted learning tables are indexed by this, so changing it silently
// remaps everything the pool has learned.
inline uint8_t plumbing_key(bool spa_suction, bool main_drain_suction,
                            bool spa_return, bool fountain_return) {
  return static_cast<uint8_t>((spa_suction ? 1u : 0u) |
                              (main_drain_suction ? 2u : 0u) |
                              (spa_return ? 4u : 0u) |
                              (fountain_return ? 8u : 0u));
}

// K = rpm / GPM for one observation. NaN when the sample cannot support it.
inline float specific_resistance(float rpm, float flow) {
  if (std::isnan(rpm) || std::isnan(flow) || rpm <= 0.0f || flow <= 0.0f)
    return std::numeric_limits<float>::quiet_NaN();
  return rpm / flow;
}

// Whether an observation may be learned from.
//
// Rejects samples taken AT AN RPM CLAMP: if the loop is pinned at rpm_min or
// rpm_max then rpm is no longer the value that produced this flow, and K would
// be learned wrong (and, being persisted, wrong for a long time). steady_count
// comes from filter_steady_count() and waits out valve travel and pump ramps.
inline bool learn_ready(bool pump_running, float rpm, float flow,
                        uint16_t steady_count, uint16_t required_count,
                        float rpm_min, float rpm_max) {
  if (!pump_running)
    return false;
  if (std::isnan(rpm) || std::isnan(flow) || rpm <= 0.0f || flow <= 0.0f)
    return false;
  if (rpm <= rpm_min || rpm >= rpm_max)
    return false;
  return steady_count >= required_count;
}

// Exponential moving average. An unset accumulator (NaN or <= 0) adopts the
// first sample outright rather than crawling up from nothing, so a newly
// learned combo is usable immediately. alpha is clamped to [0,1].
inline float ema_update(float prev, float sample, float alpha) {
  if (std::isnan(sample) || sample <= 0.0f)
    return prev;
  if (std::isnan(prev) || prev <= 0.0f)
    return sample;
  if (alpha < 0.0f) alpha = 0.0f;
  if (alpha > 1.0f) alpha = 1.0f;
  return prev + alpha * (sample - prev);
}

// Split an observation on a NON-reference combo into geometry, given the
// current global filter factor.
inline float geometry_from_observation(float k_obs, float f_load) {
  if (std::isnan(k_obs) || std::isnan(f_load) || f_load <= 0.0f)
    return std::numeric_limits<float>::quiet_NaN();
  return k_obs / f_load;
}

// Split an observation on the REFERENCE combo into the global filter factor,
// given that combo's clean-baseline geometry.
inline float load_from_observation(float k_obs, float g_reference) {
  if (std::isnan(k_obs) || std::isnan(g_reference) || g_reference <= 0.0f)
    return std::numeric_limits<float>::quiet_NaN();
  return k_obs / g_reference;
}

// Reject implausible jumps in the filter factor before they are learned.
//
// F is supposed to creep over weeks. A step change is far more likely a stuck
// diverter, a closed valve or a bad telemetry frame than sudden sand loading,
// and letting it in would corrupt every combo at once (F is global) and
// command a large rpm change. Callers should surface a rejection rather than
// swallow it. A first reading (prev unset) is always accepted.
inline bool load_plausible(float f_new, float f_prev, float max_rel_step) {
  if (std::isnan(f_new) || f_new <= 0.0f)
    return false;
  if (std::isnan(f_prev) || f_prev <= 0.0f)
    return true;
  return std::fabs(f_new - f_prev) <= max_rel_step * f_prev;
}

// Feed-forward: the rpm this combo should need for a target flow, from what we
// have already learned. Used on entry to a combo, so it starts at roughly the
// right speed instead of converging from a default every time.
inline float rpm_for_flow(float g_combo, float f_load, float target_flow,
                          float rpm_min, float rpm_max) {
  if (std::isnan(g_combo) || std::isnan(f_load) || std::isnan(target_flow) ||
      g_combo <= 0.0f || f_load <= 0.0f || target_flow <= 0.0f)
    return std::numeric_limits<float>::quiet_NaN();
  float rpm = g_combo * f_load * target_flow;
  if (rpm < rpm_min) rpm = rpm_min;
  if (rpm > rpm_max) rpm = rpm_max;
  return rpm;
}

// One correction step. Returns the rpm to command now.
//
// PROPORTIONAL, with a gain in rpm per GPM of error:
//
//     rpm_next = rpm_now + gain * (target - flow_avg)
//
// The gain must be the LOCAL SLOPE d(rpm)/d(flow), not rpm/flow. Those are only
// the same if flow is proportional to rpm, and on this pool it is not -- a
// least-squares fit of the 2026-07-26 sweep gives
//
//     flow = 0.03221 * rpm - 26.20      (R^2 = 0.984, n = 8, 1150-1450 rpm)
//     flow = 0.02934 * rpm - 22.61      (R^2 = 0.987, n = 14, every flowing point)
//
// so the true gain is ~31 rpm/GPM and roughly CONSTANT, while rpm/flow is 82 at
// the holding point and climbs as rpm falls. Using rpm/flow therefore overshoots
// by ~2.6x. The large negative intercept is static head: this pool needs ~815
// rpm before it moves any water at all, which the sweep confirmed independently
// (~1 GPM at 800 rpm, where the fit predicts ~0). That is also why K =
// specific_resistance() is not constant -- the relationship is affine, not
// proportional, so a secant through the origin is always wrong and wrong by a
// varying amount.
//
// Prefer a gain slightly BELOW the measured slope: under-gain converges
// monotonically in an extra step or two, over-gain rings.
//
// Then:
//   - DEADBAND, which should be half the flow estimate's ~1 GPM quantisation.
//     Below that, the error is indistinguishable from zero (a reading of 16
//     means the truth is somewhere in [15.5, 16.5)), so acting on it is acting
//     on a number that was never measured. It is also what stops a limit cycle
//     on a non-integer target, where the reading can only ever straddle it.
//   - STEP CLAMP, bounding how far one bad average can move the pump. With a
//     correct gain this only binds on large errors (establishment), instead of
//     being the thing that silently does the controlling.
//   - RANGE CLAMP to the caller's rpm limits.
// Returns rpm_now unchanged when the inputs cannot support a decision.
inline float loop_step(float rpm_now, float flow_avg, float target_flow,
                       float gain_rpm_per_gpm, float deadband_gpm,
                       float max_step_rpm, float rpm_min, float rpm_max) {
  if (std::isnan(rpm_now) || std::isnan(flow_avg) || std::isnan(target_flow) ||
      std::isnan(gain_rpm_per_gpm) || rpm_now <= 0.0f || flow_avg <= 0.0f ||
      target_flow <= 0.0f || gain_rpm_per_gpm <= 0.0f)
    return rpm_now;
  if (std::fabs(flow_avg - target_flow) <= deadband_gpm)
    return rpm_now;

  float rpm_next = rpm_now + gain_rpm_per_gpm * (target_flow - flow_avg);

  const float delta = rpm_next - rpm_now;
  if (delta > max_step_rpm)
    rpm_next = rpm_now + max_step_rpm;
  else if (delta < -max_step_rpm)
    rpm_next = rpm_now - max_step_rpm;

  if (rpm_next < rpm_min) rpm_next = rpm_min;
  if (rpm_next > rpm_max) rpm_next = rpm_max;
  return rpm_next;
}

// Filter load as a percentage, from the global factor. Mirrors
// filter_load_pct()'s contract (NaN when unknown) but is combo-independent.
//
// NOTE filter_load_pct() is not restricted by control MODE -- it is restricted
// by FLOW: it returns NaN once flow drifts more than 10% from the baseline
// flow. Holding a steady flow in rpm mode keeps it perfectly valid, which is
// why this function is currently unused.
inline float filter_load_pct_from_factor(float f_load) {
  if (std::isnan(f_load) || f_load <= 0.0f)
    return std::numeric_limits<float>::quiet_NaN();
  return 100.0f * f_load;
}

}  // namespace pool_control
}  // namespace esphome
