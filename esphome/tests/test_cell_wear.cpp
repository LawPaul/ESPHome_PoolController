// Host unit tests for the salt-cell runtime accounting.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_cell_wear.cpp -o /tmp/cwtest && /tmp/cwtest
#include "../components/pool_control/cell_wear.h"

#include <cassert>
#include <cmath>
#include <cstdio>

using esphome::pool_control::cell_wear_step;

static bool near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main() {
  // --- cell_wear_step ---
  // Full output for one hour -> 1 h wall-clock and 1 h equivalent.
  {
    auto s = cell_wear_step(100.0f, 3600.0f);
    assert(near(s.gen_delta_h, 1.0f));
    assert(near(s.equiv_delta_h, 1.0f));
  }
  // Half output for one hour -> 1 h wall-clock but 0.5 h equivalent.
  {
    auto s = cell_wear_step(50.0f, 3600.0f);
    assert(near(s.gen_delta_h, 1.0f));
    assert(near(s.equiv_delta_h, 0.5f));
  }
  // 60 s window at 20% -> proportional deltas.
  {
    auto s = cell_wear_step(20.0f, 60.0f);
    assert(near(s.gen_delta_h, 60.0f / 3600.0f));
    assert(near(s.equiv_delta_h, (60.0f / 3600.0f) * 0.2f));
  }
  // Idle / invalid inputs -> zero deltas.
  {
    auto s0 = cell_wear_step(0.0f, 3600.0f);
    assert(near(s0.gen_delta_h, 0.0f) && near(s0.equiv_delta_h, 0.0f));
    auto sneg = cell_wear_step(-5.0f, 3600.0f);
    assert(near(sneg.gen_delta_h, 0.0f) && near(sneg.equiv_delta_h, 0.0f));
    auto snan = cell_wear_step(std::nanf(""), 3600.0f);
    assert(near(snan.gen_delta_h, 0.0f) && near(snan.equiv_delta_h, 0.0f));
    auto sdt = cell_wear_step(100.0f, 0.0f);
    assert(near(sdt.gen_delta_h, 0.0f) && near(sdt.equiv_delta_h, 0.0f));
  }

  std::printf("cell_wear: all tests passed\n");
  return 0;
}
