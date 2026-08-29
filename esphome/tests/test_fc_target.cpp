// Host unit tests for the FC-target-from-CYA chemistry bound.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_fc_target.cpp -o /tmp/fctest && /tmp/fctest
#include "../components/pool_control/fc_target.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::fc_target_from_cya;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main() {
  const float ratio = 0.075f;  // TFP SWG ratio
  const float lo = 3.0f, hi = 10.0f;

  // Mid-range: 60 ppm CYA * 0.075 = 4.5 ppm (within band).
  assert(near(fc_target_from_cya(60.0f, ratio, lo, hi), 4.5f));

  // Low CYA hits the safety floor: 20 * 0.075 = 1.5 -> clamped to 3.
  assert(near(fc_target_from_cya(20.0f, ratio, lo, hi), lo));

  // High CYA hits the ceiling: 200 * 0.075 = 15 -> clamped to 10.
  assert(near(fc_target_from_cya(200.0f, ratio, lo, hi), hi));

  // Exactly at the floor value passes through.
  assert(near(fc_target_from_cya(40.0f, ratio, lo, hi), lo));  // 3.0

  // Missing CYA -> NaN.
  assert(std::isnan(fc_target_from_cya(std::nanf(""), ratio, lo, hi)));

  std::printf("fc_target: all tests passed\n");
  return 0;
}
