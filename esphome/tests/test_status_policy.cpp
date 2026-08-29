// Host unit tests for the status-LED severity precedence.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_status_policy.cpp -o /tmp/sptest && /tmp/sptest
#include "../components/pool_control/status_policy.h"

#include <cassert>
#include <cstdio>

using namespace esphome::pool_control;

int main() {
  // All clear -> healthy.
  assert(status_state(false, false, false, false, false, false, false) == STATUS_HEALTHY);

  // Each state in isolation.
  assert(status_state(true, false, false, false, false, false, false) == STATUS_SERVICE);
  assert(status_state(false, true, false, false, false, false, false) == STATUS_COMMS);
  assert(status_state(false, false, true, false, false, false, false) == STATUS_ALARM);
  assert(status_state(false, false, false, true, false, false, false) == STATUS_FREEZE);
  assert(status_state(false, false, false, false, true, false, false) == STATUS_CLEANING);
  assert(status_state(false, false, false, false, false, true, false) == STATUS_BOOST);
  assert(status_state(false, false, false, false, false, false, true) == STATUS_WARN);

  // Precedence: service outranks everything, even with all flags set.
  assert(status_state(true, true, true, true, true, true, true) == STATUS_SERVICE);
  // Comms outranks alarm (a stale link makes the alarm untrustworthy).
  assert(status_state(false, true, true, true, true, true, true) == STATUS_COMMS);
  // Alarm outranks freeze/cleaning/boost/warn.
  assert(status_state(false, false, true, true, true, true, true) == STATUS_ALARM);
  // Freeze outranks cleaning/boost/warn.
  assert(status_state(false, false, false, true, true, true, true) == STATUS_FREEZE);
  // Cleaning outranks boost/warn.
  assert(status_state(false, false, false, false, true, true, true) == STATUS_CLEANING);
  // Boost outranks warn.
  assert(status_state(false, false, false, false, false, true, true) == STATUS_BOOST);

  std::printf("status_policy: all tests passed\n");
  return 0;
}
