#pragma once

#include <cstdint>

namespace esphome {
namespace pool_control {

// Pure staleness watchdog for a periodically-polled RS485 peer (pump / salt
// cell). Framework-free so the grace + timeout deadline logic is host-tested;
// the YAML binary_sensor lambdas only read the timestamps and call this.
//
//   now_ms           - current millis()
//   powered_since_ms  - millis() when the peer was powered on (0 = unpowered)
//   last_seen_ms      - millis() of the last fresh telemetry frame
//   grace_ms          - settle window after power-on before comms are expected
//   timeout_ms        - max silence before the link is declared lost
//
// Returns false while unpowered or inside the grace window (no comms expected),
// then true once telemetry is older than the timeout.
inline bool comms_lost(uint32_t now_ms, uint32_t powered_since_ms,
                       uint32_t last_seen_ms, uint32_t grace_ms,
                       uint32_t timeout_ms) {
  if (powered_since_ms == 0)
    return false;
  if (now_ms - powered_since_ms < grace_ms)
    return false;
  return (now_ms - last_seen_ms) > timeout_ms;
}

}  // namespace pool_control
}  // namespace esphome
