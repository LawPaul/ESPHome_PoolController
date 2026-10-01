// Host unit tests for the RS485 comms staleness watchdog.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_comms_watchdog.cpp -o /tmp/cwdtest && /tmp/cwdtest
#include "../components/pool_control/comms_watchdog.h"

#include <cassert>
#include <cstdio>

using esphome::pool_control::comms_lost;
using esphome::pool_control::comms_lost_latched;
using esphome::pool_control::StalenessLatch;

int main() {
  const uint32_t grace = 90000;    // 90 s after power-on
  const uint32_t timeout = 90000;  // ~3 missed 30 s polls

  // Unpowered (powered_since == 0) -> never lost, no comms expected.
  assert(!comms_lost(/*now*/500000, /*powered*/0, /*seen*/0, grace, timeout));

  // Inside the post-power-on grace window -> not lost even with no telemetry.
  assert(!comms_lost(/*now*/50000, /*powered*/0 + 1, /*seen*/1, grace, timeout));
  // now - powered = 89999 (< grace) -> still not lost.
  assert(!comms_lost(/*now*/90000, /*powered*/1, /*seen*/1, grace, timeout));

  // Past grace, telemetry fresh -> healthy.
  assert(!comms_lost(/*now*/200000, /*powered*/1, /*seen*/199000, grace, timeout));

  // Past grace, telemetry stale beyond timeout -> lost.
  assert(comms_lost(/*now*/300000, /*powered*/1, /*seen*/200000, grace, timeout));

  // Exactly at the timeout boundary -> not yet lost (strictly greater).
  assert(!comms_lost(/*now*/1, /*powered*/0, /*seen*/0, grace, timeout));  // unpowered guard
  // now-seen == timeout exactly -> not lost.
  {
    uint32_t seen = 210000;
    uint32_t now = seen + timeout;  // == timeout
    assert(!comms_lost(now, /*powered*/1, seen, grace, timeout));
    assert(comms_lost(now + 1, /*powered*/1, seen, grace, timeout));  // one past
  }

  // A new sample, not clock rollover, clears stale state.
  {
    StalenessLatch latch;
    assert(comms_lost_latched(latch, 300000, 1, 100000, grace, timeout));
    assert(comms_lost_latched(latch, 1000, 1, 100000, grace, timeout));
    assert(!comms_lost_latched(latch, 2000, 1, 2000, grace, timeout));
  }

  std::printf("comms_watchdog: all tests passed\n");
  return 0;
}
