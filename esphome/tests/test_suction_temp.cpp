// Host unit tests for the suction-source demux + settle.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_suction_temp.cpp -o /tmp/sttest && /tmp/sttest
#include "../components/pool_control/suction_temp.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::suction_source;
using esphome::pool_control::suction_settled;
using esphome::pool_control::settle_ms_for_gallons;
using esphome::pool_control::SUCTION_SKIMMER;
using esphome::pool_control::SUCTION_DRAIN;
using esphome::pool_control::SUCTION_SPA;

int main() {
  // --- suction_source: valve1 (spa) overrides valve2 (drain) ---
  {
    assert(suction_source(/*spa*/false, /*drain*/false) == SUCTION_SKIMMER);
    assert(suction_source(/*spa*/false, /*drain*/true) == SUCTION_DRAIN);
    assert(suction_source(/*spa*/true, /*drain*/false) == SUCTION_SPA);
    // Spa wins even if the pool-loop sub-select is also set.
    assert(suction_source(/*spa*/true, /*drain*/true) == SUCTION_SPA);
  }

  // --- suction_settled ---
  {
    const uint32_t settle = 90000;  // 90 s
    assert(!suction_settled(/*now*/1000, /*changed*/0, settle));      // 1 s < 90 s
    assert(!suction_settled(/*now*/89999, /*changed*/0, settle));     // just under
    assert(suction_settled(/*now*/90000, /*changed*/0, settle));      // exactly at
    assert(suction_settled(/*now*/120000, /*changed*/0, settle));     // well past
  }

  // --- settle_ms_for_gallons: time = gallons / live flow ---
  {
    // 40 gal at 40 GPM = 1 min; at 20 GPM = 2 min; at 80 GPM = 0.5 min.
    assert(settle_ms_for_gallons(40.0f, 40.0f) == 60000);
    assert(settle_ms_for_gallons(40.0f, 20.0f) == 120000);
    assert(settle_ms_for_gallons(40.0f, 80.0f) == 30000);
    // Slower pump waits longer for the same gallons (monotonic in 1/flow).
    assert(settle_ms_for_gallons(40.0f, 28.0f) > settle_ms_for_gallons(40.0f, 50.0f));
    // Flow floored at 10 GPM: zero / sub-floor / NaN can't run away.
    assert(settle_ms_for_gallons(40.0f, 0.0f) == 240000);        // -> 10 GPM = 4 min
    assert(settle_ms_for_gallons(40.0f, 5.0f) == 240000);        // sub-floor -> 10 GPM
    assert(settle_ms_for_gallons(40.0f, std::nanf("")) == 240000);
  }

  std::printf("suction_temp: all tests passed\n");
  return 0;
}
