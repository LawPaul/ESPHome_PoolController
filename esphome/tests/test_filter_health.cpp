// Host unit tests for the filter-load estimate + clean-needed threshold.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_filter_health.cpp -o /tmp/fhtest && /tmp/fhtest
#include "../components/pool_control/filter_health.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::filter_load_pct;
using esphome::pool_control::filter_dirt_pct;
using esphome::pool_control::filter_clean_needed;
using esphome::pool_control::filter_reference_plumbing;
using esphome::pool_control::filter_steady_count;
using esphome::pool_control::filter_capture_ready;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main() {
  const float base_rpm = 2000.0f, base_flow = 10.0f;

  // --- filter_load_pct ---
  {
    // At baseline rpm + flow -> 100%.
    assert(near(filter_load_pct(2000.0f, 10.0f, base_rpm, base_flow), 100.0f));
    // Loaded: needs 2400 rpm to hold flow -> 120%.
    assert(near(filter_load_pct(2400.0f, 10.0f, base_rpm, base_flow), 120.0f));
    // Flow within 10% of reference still valid (10.9 is +9%).
    assert(!std::isnan(filter_load_pct(2100.0f, 10.9f, base_rpm, base_flow)));
    // Flow off reference (>10%) -> NaN (not comparable hydraulics).
    assert(std::isnan(filter_load_pct(2100.0f, 12.0f, base_rpm, base_flow)));
    // No baseline captured -> NaN.
    assert(std::isnan(filter_load_pct(2000.0f, 10.0f, 0.0f, base_flow)));
    assert(std::isnan(filter_load_pct(2000.0f, 10.0f, base_rpm, 0.0f)));
    // Missing telemetry -> NaN.
    assert(std::isnan(filter_load_pct(std::nanf(""), 10.0f, base_rpm, base_flow)));
    assert(std::isnan(filter_load_pct(2000.0f, std::nanf(""), base_rpm, base_flow)));
  }

  // --- filter_clean_needed ---
  {
    const float threshold = 120.0f;
    assert(!filter_clean_needed(100.0f, threshold));   // clean
    assert(!filter_clean_needed(120.0f, threshold));   // exactly at -> not yet
    assert(filter_clean_needed(125.0f, threshold));    // above -> needs clean
    assert(!filter_clean_needed(std::nanf(""), threshold));  // no reading -> never
  }

  // --- filter_dirt_pct ---
  {
    const float thr = 120.0f;
    // Endpoints: baseline rpm reads clean, the trip point reads full scale.
    assert(near(filter_dirt_pct(100.0f, thr), 0.0f));
    assert(near(filter_dirt_pct(120.0f, thr), 100.0f));
    // Linear in between, and unclamped above so overdue keeps climbing.
    assert(near(filter_dirt_pct(110.0f, thr), 50.0f));
    assert(near(filter_dirt_pct(130.0f, thr), 150.0f));
    // Below baseline is noise/cooler water, not cleaner-than-clean.
    assert(near(filter_dirt_pct(97.0f, thr), 0.0f));
    // A tighter threshold makes the same rpm rise read dirtier.
    assert(near(filter_dirt_pct(110.0f, 110.0f), 100.0f));
    // Unusable inputs -> NaN, never a number.
    assert(std::isnan(filter_dirt_pct(std::nanf(""), thr)));
    assert(std::isnan(filter_dirt_pct(110.0f, std::nanf(""))));
    assert(std::isnan(filter_dirt_pct(110.0f, 100.0f)));  // zero-width scale
    assert(std::isnan(filter_dirt_pct(110.0f, 90.0f)));   // inverted scale

    // The rescale is monotonic, so tripping on dirt > 100 is the same decision
    // as tripping on raw load > threshold -- which is what the YAML now does.
    for (float load = 95.0f; load <= 140.0f; load += 0.5f)
      assert(filter_clean_needed(filter_dirt_pct(load, thr), 100.0f) ==
             filter_clean_needed(load, thr));
  }

  // --- filter_reference_plumbing ---
  {
    // Skimmer suction + pool-jet return = all four diverters de-energised.
    assert(filter_reference_plumbing(false, false, false, false));
    // Any diverted path changes the system curve -> not comparable.
    assert(!filter_reference_plumbing(true, false, false, false));   // spa suction
    assert(!filter_reference_plumbing(false, true, false, false));   // main drain
    assert(!filter_reference_plumbing(false, false, true, false));   // spa return
    assert(!filter_reference_plumbing(false, false, false, true));   // fountains
  }

  // --- filter_steady_count ---
  {
    const float tol = 25.0f;
    // First sample has no predecessor (-1 sentinel) -> restart.
    assert(filter_steady_count(1500.0f, -1.0f, 0, tol) == 0);
    // Holding still accumulates.
    assert(filter_steady_count(1500.0f, 1500.0f, 0, tol) == 1);
    assert(filter_steady_count(1510.0f, 1500.0f, 3, tol) == 4);
    // Exactly at tolerance still counts as steady.
    assert(filter_steady_count(1525.0f, 1500.0f, 3, tol) == 4);
    // An excursion (valve travel, pump ramp) restarts the window.
    assert(filter_steady_count(1800.0f, 1500.0f, 5, tol) == 0);
    // Unusable readings restart it too.
    assert(filter_steady_count(std::nanf(""), 1500.0f, 5, tol) == 0);
    assert(filter_steady_count(1500.0f, std::nanf(""), 5, tol) == 0);
    assert(filter_steady_count(0.0f, 1500.0f, 5, tol) == 0);
    // Saturates instead of wrapping back to 0.
    assert(filter_steady_count(1500.0f, 1500.0f, 0xFFFFu, tol) == 0xFFFFu);
  }

  // --- filter_capture_ready ---
  {
    const uint16_t need = 6;
    // Everything satisfied -> capture.
    assert(filter_capture_ready(true, true, true, true, true, 1500.0f, 20.0f, 6, need));
    assert(filter_capture_ready(true, true, true, true, true, 1500.0f, 20.0f, 9, need));
    // Not armed -> never captures on its own.
    assert(!filter_capture_ready(false, true, true, true, true, 1500.0f, 20.0f, 9, need));
    // Armed but still settling.
    assert(!filter_capture_ready(true, true, true, true, true, 1500.0f, 20.0f, 5, need));
    // Armed from service mode: waits for the equipment to come back.
    assert(!filter_capture_ready(true, true, false, true, true, 1500.0f, 20.0f, 9, need));
    // Wrong plumbing (e.g. main drain leg) -> waits for the reference combo.
    assert(!filter_capture_ready(true, true, true, false, true, 1500.0f, 20.0f, 9, need));
    // Pump stopped -> nothing to measure.
    assert(!filter_capture_ready(true, false, true, true, true, 1500.0f, 20.0f, 9, need));
    // Rock steady, right plumbing, but the holding loop says this is a
    // transient (it is establishing at 20 GPM on the way to a lower target).
    // Stillness cannot see that, which is the whole point of the flag.
    assert(!filter_capture_ready(true, true, true, true, false, 1500.0f, 20.0f, 9, need));
    // Unusable telemetry -> no capture.
    assert(!filter_capture_ready(true, true, true, true, true, std::nanf(""), 20.0f, 9, need));
    assert(!filter_capture_ready(true, true, true, true, true, 1500.0f, std::nanf(""), 9, need));
    assert(!filter_capture_ready(true, true, true, true, true, 1500.0f, 0.0f, 9, need));
  }

  std::printf("filter_health: all tests passed\n");
  return 0;
}
