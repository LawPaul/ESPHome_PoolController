// Host unit tests for the holding-pattern suction rotation.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_holding_pattern.cpp -o /tmp/hptest && /tmp/hptest
#include "../components/pool_control/holding_pattern.h"

#include <cassert>
#include <cstdio>

using esphome::pool_control::holding_drain_now;
using esphome::pool_control::holding_rpm_for_suction;

int main() {
  const int H = 3600;
  // 2 h drain every 6 h: drain windows at [0,2), [6,8), [12,14), [18,20).
  const int period = 6 * H;
  const int drain = 2 * H;

  // Start of a period -> drain.
  assert(holding_drain_now(0, period, drain));
  assert(holding_drain_now(1 * H + 59 * 60, period, drain));  // 01:59 -> drain
  // Just past the drain window -> skimmer.
  assert(!holding_drain_now(2 * H, period, drain));            // 02:00 -> skimmer
  assert(!holding_drain_now(5 * H, period, drain));            // 05:00 -> skimmer
  // Next period rolls back to drain.
  assert(holding_drain_now(6 * H, period, drain));             // 06:00 -> drain
  assert(holding_drain_now(7 * H + 30 * 60, period, drain));   // 07:30 -> drain
  assert(!holding_drain_now(8 * H, period, drain));            // 08:00 -> skimmer
  // Afternoon window.
  assert(holding_drain_now(12 * H, period, drain));            // 12:00 -> drain
  assert(holding_drain_now(18 * H, period, drain));            // 18:00 -> drain
  assert(!holding_drain_now(20 * H, period, drain));           // 20:00 -> skimmer
  assert(!holding_drain_now(23 * H, period, drain));           // 23:00 -> skimmer

  // Degenerate configs never call for drain.
  assert(!holding_drain_now(0, 0, drain));
  assert(!holding_drain_now(0, period, 0));
  assert(!holding_drain_now(0, -1, drain));
  assert(!holding_drain_now(0, period, -5));
  // drain >= period -> always drain.
  assert(holding_drain_now(0, H, 10 * H));
  assert(holding_drain_now(H / 2, H, H));

  // Negative second-of-day is wrapped, not UB.
  assert(holding_drain_now(-1, period, drain) == holding_drain_now(period - 1, period, drain));

  // --- holding_rpm_for_suction: the drain leg gets its own (higher) setpoint ---
  {
    // Measured equal-flow points on this system (20 GPM each).
    const float skim = 1481.0f, drn = 1791.0f;
    assert(holding_rpm_for_suction(skim, drn, /*on_drain*/ false) == skim);
    assert(holding_rpm_for_suction(skim, drn, /*on_drain*/ true) == drn);
    // Selection only -- it never averages, clamps, or reorders the two. A drain
    // setpoint BELOW the skimmer one is honoured as configured (the rpm range
    // clamp is rpm_to_cmd's job, and the ordering is the installer's call).
    assert(holding_rpm_for_suction(1600.0f, 1200.0f, true) == 1200.0f);
    assert(holding_rpm_for_suction(1600.0f, 1200.0f, false) == 1600.0f);
  }

  std::printf("All holding pattern tests passed.\n");
  return 0;
}
