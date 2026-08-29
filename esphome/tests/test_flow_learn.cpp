// Host unit tests for the rpm-mode flow-holding loop + per-combo learning.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_flow_learn.cpp -o /tmp/fltest && /tmp/fltest
#include "../components/pool_control/flow_learn.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::NUM_PLUMBING_KEYS;
using esphome::pool_control::plumbing_key;
using esphome::pool_control::specific_resistance;
using esphome::pool_control::learn_ready;
using esphome::pool_control::ema_update;
using esphome::pool_control::geometry_from_observation;
using esphome::pool_control::load_from_observation;
using esphome::pool_control::load_plausible;
using esphome::pool_control::rpm_for_flow;
using esphome::pool_control::loop_step;
using esphome::pool_control::filter_load_pct_from_factor;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

int main() {
  // --- plumbing_key ---
  {
    // Reference combo (all diverters off) must be index 0.
    assert(plumbing_key(false, false, false, false) == 0);
    // Each diverter owns exactly one bit, in the documented order.
    assert(plumbing_key(true, false, false, false) == 1);
    assert(plumbing_key(false, true, false, false) == 2);
    assert(plumbing_key(false, false, true, false) == 4);
    assert(plumbing_key(false, false, false, true) == 8);
    assert(plumbing_key(true, true, true, true) == 15);
    // Every key is a valid table index.
    assert(plumbing_key(true, true, true, true) < NUM_PLUMBING_KEYS);

    // The two combos we actually care about are distinct: idle pool skimmer
    // (skimmer suction, pool return) vs cleaning spillover (skimmer suction,
    // SPA return). Same suction, different return -> different geometry.
    const uint8_t idle_skimmer = plumbing_key(false, false, false, false);
    const uint8_t spillover = plumbing_key(false, false, true, false);
    assert(idle_skimmer != spillover);
  }

  // --- specific_resistance ---
  {
    // Measured points from this pool: 20 GPM at 1489 rpm on the skimmer leg,
    // 1791 on the main drain leg.
    assert(near(specific_resistance(1489.0f, 20.0f), 74.45f));
    assert(near(specific_resistance(1791.0f, 20.0f), 89.55f));
    // Unusable samples -> NaN.
    assert(std::isnan(specific_resistance(0.0f, 20.0f)));
    assert(std::isnan(specific_resistance(1489.0f, 0.0f)));
    assert(std::isnan(specific_resistance(NAN, 20.0f)));
    assert(std::isnan(specific_resistance(1489.0f, NAN)));
  }

  // --- learn_ready ---
  {
    const float lo = 600.0f, hi = 3450.0f;
    assert(learn_ready(true, 1500.0f, 20.0f, 10, 5, lo, hi));
    // Pump stopped, or telemetry missing/zero.
    assert(!learn_ready(false, 1500.0f, 20.0f, 10, 5, lo, hi));
    assert(!learn_ready(true, NAN, 20.0f, 10, 5, lo, hi));
    assert(!learn_ready(true, 1500.0f, 0.0f, 10, 5, lo, hi));
    // Not settled yet (valve travel / pump ramp).
    assert(!learn_ready(true, 1500.0f, 20.0f, 4, 5, lo, hi));
    // AT A CLAMP: rpm is not what produced this flow, so K would be wrong.
    assert(!learn_ready(true, lo, 20.0f, 10, 5, lo, hi));
    assert(!learn_ready(true, hi, 20.0f, 10, 5, lo, hi));
    // Just inside the clamps is fine.
    assert(learn_ready(true, lo + 1.0f, 20.0f, 10, 5, lo, hi));
  }

  // --- ema_update ---
  {
    // Unset accumulator adopts the first sample outright.
    assert(near(ema_update(NAN, 74.5f, 0.2f), 74.5f));
    assert(near(ema_update(0.0f, 74.5f, 0.2f), 74.5f));
    // Normal blend.
    assert(near(ema_update(100.0f, 200.0f, 0.25f), 125.0f));
    // alpha clamped.
    assert(near(ema_update(100.0f, 200.0f, 5.0f), 200.0f));
    assert(near(ema_update(100.0f, 200.0f, -1.0f), 100.0f));
    // A bad sample leaves the accumulator alone.
    assert(near(ema_update(100.0f, NAN, 0.25f), 100.0f));
    assert(near(ema_update(100.0f, -5.0f, 0.25f), 100.0f));
  }

  // --- geometry / load decomposition ---
  {
    const float g_ref = 74.5f;

    // Clean filter on the reference combo -> F == 1.
    assert(near(load_from_observation(74.5f, g_ref), 1.0f));
    // 20% loaded: same flow now costs 20% more rpm.
    assert(near(load_from_observation(89.4f, g_ref), 1.2f));

    // THE POINT OF THE SPLIT: a combo observed while the filter is dirty must
    // still yield its CLEAN geometry, so it stays valid as F moves on.
    const float g_spillover_clean = 110.0f;
    const float f_now = 1.2f;
    const float k_obs = g_spillover_clean * f_now;  // what we'd measure today
    assert(near(geometry_from_observation(k_obs, f_now), g_spillover_clean));

    // ...and that stored geometry, replayed at a LATER, dirtier F, commands
    // the higher rpm the dirtier filter needs -- without re-running the combo.
    const float f_later = 1.35f;
    assert(near(rpm_for_flow(g_spillover_clean, f_later, 12.0f, 600.0f, 3450.0f),
                110.0f * 1.35f * 12.0f));

    // Guards.
    assert(std::isnan(geometry_from_observation(NAN, 1.0f)));
    assert(std::isnan(geometry_from_observation(90.0f, 0.0f)));
    assert(std::isnan(load_from_observation(90.0f, 0.0f)));
  }

  // --- load_plausible ---
  {
    // First reading is always accepted.
    assert(load_plausible(1.0f, NAN, 0.10f));
    assert(load_plausible(1.0f, 0.0f, 0.10f));
    // Weeks-scale creep passes.
    assert(load_plausible(1.05f, 1.0f, 0.10f));
    assert(load_plausible(0.96f, 1.0f, 0.10f));
    // A step change (stuck diverter / bad frame) is rejected -- F is global,
    // so letting it in would corrupt every combo at once.
    assert(!load_plausible(1.5f, 1.0f, 0.10f));
    assert(!load_plausible(0.5f, 1.0f, 0.10f));
    // Nonsense rejected.
    assert(!load_plausible(NAN, 1.0f, 0.10f));
    assert(!load_plausible(0.0f, 1.0f, 0.10f));
  }

  // --- rpm_for_flow ---
  {
    // Clean skimmer leg, 20 GPM -> the measured ~1489 rpm.
    assert(near(rpm_for_flow(74.45f, 1.0f, 20.0f, 600.0f, 3450.0f), 1489.0f));
    // Target 12 GPM (under the flow-mode floor -- the whole reason for rpm
    // mode) -> ~893 rpm.
    assert(near(rpm_for_flow(74.45f, 1.0f, 12.0f, 600.0f, 3450.0f), 893.4f));
    // Clamped both ends.
    assert(near(rpm_for_flow(74.45f, 1.0f, 2.0f, 600.0f, 3450.0f), 600.0f));
    assert(near(rpm_for_flow(74.45f, 1.0f, 90.0f, 600.0f, 3450.0f), 3450.0f));
    // Guards.
    assert(std::isnan(rpm_for_flow(0.0f, 1.0f, 12.0f, 600.0f, 3450.0f)));
    assert(std::isnan(rpm_for_flow(74.45f, 0.0f, 12.0f, 600.0f, 3450.0f)));
    assert(std::isnan(rpm_for_flow(74.45f, 1.0f, NAN, 600.0f, 3450.0f)));
  }

  // --- loop_step ---
  {
    const float lo = 600.0f, hi = 3450.0f;
    const float db = 0.5f, step = 25.0f, g = 30.0f;

    // Inside the deadband -> do nothing. The deadband is half the pump's 1 GPM
    // flow-estimate quantisation: below that the error cannot be distinguished
    // from zero, so acting on it would be acting on an unmeasured number.
    assert(near(loop_step(900.0f, 12.4f, 12.0f, g, db, step, lo, hi), 900.0f));
    assert(near(loop_step(900.0f, 11.6f, 12.0f, g, db, step, lo, hi), 900.0f));

    // Flow low (filter loading) -> speed up, but only by the step clamp.
    assert(near(loop_step(900.0f, 10.0f, 12.0f, g, db, step, lo, hi), 925.0f));
    // Flow high -> slow down, likewise clamped.
    assert(near(loop_step(900.0f, 14.0f, 12.0f, g, db, step, lo, hi), 875.0f));

    // Error inside the step clamp -> the FULL proportional correction, gain
    // times error. This is the case the clamp used to hide: with the old
    // rpm/flow rescale the same 1 GPM error moved 75 rpm instead of 30.
    assert(near(loop_step(900.0f, 11.0f, 12.0f, g, db, 200.0f, lo, hi), 930.0f));
    assert(near(loop_step(900.0f, 13.0f, 12.0f, g, db, 200.0f, lo, hi), 870.0f));
    // Gain is what sets the step, so it scales linearly with it.
    assert(near(loop_step(900.0f, 11.0f, 12.0f, 60.0f, db, 200.0f, lo, hi),
                960.0f));

    // Range clamps win over the step.
    assert(near(loop_step(610.0f, 14.0f, 12.0f, g, db, step, lo, hi), lo));
    assert(near(loop_step(3440.0f, 10.0f, 12.0f, g, db, step, lo, hi), hi));

    // Unusable inputs -> hold station rather than command something wild.
    assert(near(loop_step(900.0f, NAN, 12.0f, g, db, step, lo, hi), 900.0f));
    assert(near(loop_step(900.0f, 0.0f, 12.0f, g, db, step, lo, hi), 900.0f));
    assert(near(loop_step(900.0f, 11.0f, 12.0f, NAN, db, step, lo, hi), 900.0f));
    assert(near(loop_step(900.0f, 11.0f, 12.0f, 0.0f, db, step, lo, hi), 900.0f));

    // CONVERGENCE on the AFFINE plant actually measured on this pool
    // (flow = 0.03221*rpm - 26.20, so 31.0 rpm per GPM). A gain of 30 is
    // deliberately just under that: it must converge without ever overshooting.
    {
      auto plant = [](float r) { return 0.03221f * r - 26.20f; };
      float rpm = 1450.0f;  // where establishment hands over, ~20 GPM
      float prev_err = plant(rpm) - 15.0f;
      for (int i = 0; i < 40; i++) {
        rpm = loop_step(rpm, plant(rpm), 15.0f, g, db, 50.0f, lo, hi);
        const float err = plant(rpm) - 15.0f;
        // Under-gain => monotone approach, never crossing the setpoint.
        assert(err >= -db);
        assert(err <= prev_err + 1e-3f);
        prev_err = err;
      }
      assert(std::fabs(plant(rpm) - 15.0f) <= db);
      // And it stays put.
      assert(near(loop_step(rpm, plant(rpm), 15.0f, g, db, 50.0f, lo, hi), rpm));
    }

    // CONVERGENCE on a purely proportional plant too (the old model), so the
    // controller is not secretly tuned to one plant shape.
    float rpm = 900.0f;
    const float k_dirty = 74.45f * 1.2f;
    for (int i = 0; i < 60; i++) {
      const float flow = rpm / k_dirty;
      rpm = loop_step(rpm, flow, 12.0f, g, db, step, lo, hi);
    }
    const float settled_flow = rpm / k_dirty;
    assert(std::fabs(settled_flow - 12.0f) <= db);
    // Held there: another pass moves nothing.
    assert(near(loop_step(rpm, settled_flow, 12.0f, g, db, step, lo, hi), rpm));
  }

  // --- filter_load_pct_from_factor ---
  {
    assert(near(filter_load_pct_from_factor(1.0f), 100.0f));
    assert(near(filter_load_pct_from_factor(1.2f), 120.0f));
    assert(std::isnan(filter_load_pct_from_factor(NAN)));
    assert(std::isnan(filter_load_pct_from_factor(0.0f)));
  }

  printf("test_flow_learn: all assertions passed\n");
  return 0;
}
