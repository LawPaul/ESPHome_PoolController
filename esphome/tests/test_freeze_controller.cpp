// Host unit tests for the freeze-protection decision.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_freeze_controller.cpp -o /tmp/fztest && /tmp/fztest
#include "../components/pool_control/freeze_controller.h"
#include "../components/pool_control/comms_watchdog.h"

#include <cassert>
#include <cstdio>

using esphome::pool_control::freeze_tick;
using esphome::pool_control::freeze_failsafe;
using esphome::pool_control::temp_sensor_stale;

int main() {
  const int H = 3600;
  const int rot = 5 * 60;   // 5 min phases
  const float thr = 2.0f;
  const float hys = 1.0f;
  const float normal = 44.0f;

  // --- Entry / exit with hysteresis ---
  {
    // Below threshold from inactive -> enters.
    auto d = freeze_tick(1.5f, thr, hys, /*was*/false, 0, rot, normal);
    assert(d.active && d.entered && !d.exited);
    // Forced flow floors at 48 even though normal is 44.
    assert(d.forced_flow == 48.0f);

    // Between thr and thr+hys while active -> holds active, no transition.
    d = freeze_tick(2.5f, thr, hys, /*was*/true, 0, rot, normal);
    assert(d.active && !d.entered && !d.exited);

    // Above thr+hys while active -> exits.
    d = freeze_tick(3.5f, thr, hys, /*was*/true, 0, rot, normal);
    assert(!d.active && d.exited && !d.entered);

    // Warm from inactive -> stays inactive, no transition.
    d = freeze_tick(10.0f, thr, hys, /*was*/false, 0, rot, normal);
    assert(!d.active && !d.entered && !d.exited);
  }

  // --- Forced flow follows normal_flow when it exceeds the floor ---
  {
    auto d = freeze_tick(0.0f, thr, hys, false, 0, rot, /*normal*/66.0f);
    assert(d.forced_flow == 66.0f);
  }

  // --- Rotation phases cycle skimmer -> drain -> spa ---
  {
    // phase 0: pool / skimmer
    auto d = freeze_tick(0.0f, thr, hys, true, 0, rot, normal);
    assert(d.phase == 0 && !d.valve1_spa && !d.valve3_spa && !d.valve2_drain);
    // phase 1: pool / main drain
    d = freeze_tick(0.0f, thr, hys, true, rot, rot, normal);
    assert(d.phase == 1 && !d.valve1_spa && !d.valve3_spa && d.valve2_drain);
    // phase 2: spa (valve1 and valve3 move together)
    d = freeze_tick(0.0f, thr, hys, true, 2 * rot, rot, normal);
    assert(d.phase == 2 && d.valve1_spa && d.valve3_spa);
    // wraps back to phase 0
    d = freeze_tick(0.0f, thr, hys, true, 3 * rot, rot, normal);
    assert(d.phase == 0);
    // arbitrary time of day still lands in a valid phase
    d = freeze_tick(0.0f, thr, hys, true, 13 * H + 7 * 60, rot, normal);
    assert(d.phase >= 0 && d.phase <= 2);
    // valve1 and valve3 always agree (never a spillover state)
    for (int t = 0; t < 24 * H; t += 137) {
      auto r = freeze_tick(0.0f, thr, hys, true, t, rot, normal);
      assert(r.valve1_spa == r.valve3_spa);
    }
  }

  // --- rotate_s <= 0 is safe (pins phase 0) ---
  {
    auto d = freeze_tick(0.0f, thr, hys, true, 12345, 0, normal);
    assert(d.phase == 0);
  }

  // --- Minimum run-on: hold active until BOTH temp clears AND runtime elapsed ---
  {
    const uint32_t minrt = 30u * 60u * 1000u;  // 30 min
    // Warm enough to release, but run-on not satisfied -> stays active.
    auto d = freeze_tick(3.5f, thr, hys, /*was*/true, 0, rot, normal,
                         /*elapsed*/60000u, minrt);
    assert(d.active && !d.exited);
    // Same warmth, run-on satisfied -> now releases.
    d = freeze_tick(3.5f, thr, hys, true, 0, rot, normal, minrt, minrt);
    assert(!d.active && d.exited);
    // Still cold overrides the run-on gate entirely (cold always protects).
    d = freeze_tick(1.0f, thr, hys, true, 0, rot, normal, 0u, minrt);
    assert(d.active && !d.exited);
    // min_runtime 0 (default) == pure hysteresis: warm+active -> exits at once.
    d = freeze_tick(3.5f, thr, hys, true, 0, rot, normal);
    assert(!d.active && d.exited);
  }

  // --- temp_sensor_stale: trips once now-last_seen reaches the window ---
  {
    const uint32_t win = 300000;  // 5 min
    assert(!temp_sensor_stale(/*now*/100000, /*seen*/0, win));   // 100s uptime < 5min grace
    assert(!temp_sensor_stale(299999, 0, win));                  // just under
    assert(temp_sensor_stale(300000, 0, win));                   // never-seen probe trips
    assert(!temp_sensor_stale(1000000, 800000, win));            // fresh (200s ago)
    assert(temp_sensor_stale(1000000, 600000, win));             // 400s ago -> stale

    esphome::pool_control::StalenessLatch latch;
    assert(latch.update(600000, temp_sensor_stale(1000000, 600000, win)));
    assert(latch.update(600000, temp_sensor_stale(1000, 600000, win)));
    assert(!latch.update(1000, temp_sensor_stale(1000, 1000, win)));
  }

  // --- freeze_failsafe: always protects, mirrors a real freeze tick ---
  {
    // Enters protection from inactive regardless of (absent) temperature.
    auto d = freeze_failsafe(thr, hys, /*was*/false, 0, rot, normal);
    assert(d.active && d.entered && d.forced_flow == 48.0f);
    // Rotation still cycles so no loop sits static during a sensor fault.
    auto d0 = freeze_failsafe(thr, hys, true, 0, rot, normal);
    auto d1 = freeze_failsafe(thr, hys, true, rot, rot, normal);
    auto d2 = freeze_failsafe(thr, hys, true, 2 * rot, rot, normal);
    assert(d0.phase == 0 && d1.phase == 1 && d2.phase == 2);
    assert(d2.valve1_spa && d2.valve3_spa);
    // Never spuriously reports an exit (can't leave protection on a dead sensor).
    assert(!d0.exited && !d1.exited && !d2.exited);
  }

  printf("All freeze controller tests passed.\n");
  return 0;
}
