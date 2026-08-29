#pragma once

#include <cmath>
#include <cstdint>

namespace esphome {
namespace pool_control {

// Pure suction-source demux. The cell's water_temp reflects whichever suction
// diverter is selected; this maps the two diverter relay states to a source and
// answers the flush-settle question. Framework-free so it is host-tested; the
// YAML lambda handles the sensor reads, gating (pump/cell live) and publishing.

enum SuctionSource {
  SUCTION_SKIMMER = 0,  // surface water
  SUCTION_DRAIN = 1,    // main drain (deep)
  SUCTION_SPA = 2,      // spa
};

// Which source feeds the cell right now. valve1 ON = spa suction and overrides
// the pool-loop sub-select; else valve2 ON = main drain; else skimmer.
inline int suction_source(bool valve1_spa, bool valve2_drain) {
  if (valve1_spa)
    return SUCTION_SPA;
  if (valve2_drain)
    return SUCTION_DRAIN;
  return SUCTION_SKIMMER;
}

// Has the selected source been stable long enough for the pipe + cell to flush
// to the new water, so the measured temperature is representative?
inline bool suction_settled(uint32_t now_ms, uint32_t changed_ms,
                            uint32_t settle_ms) {
  return (now_ms - changed_ms) >= settle_ms;
}

// Settling is really a flushed-VOLUME problem, not a fixed time: the new water
// must displace the volume between the suction source and the probe (the sand
// filter dominates, plus pump + pipes). Convert a gallons target into a settle
// time for the CURRENT pump flow, so a slower pump waits proportionally longer
// for the same volume to pass. `flow_gpm` is floored to a small positive value
// so a zero / stale / NaN reading can't produce an unbounded wait (the caller
// also gates on pump-running, where real flow is always well above the floor).
inline uint32_t settle_ms_for_gallons(float gallons, float flow_gpm) {
  float f = flow_gpm;
  if (std::isnan(f) || f < 10.0f)
    f = 10.0f;                             // floor: bound the worst-case wait
  float minutes = gallons / f;             // gal / (gal/min) = minutes
  return (uint32_t) (minutes * 60000.0f);  // -> milliseconds
}

}  // namespace pool_control
}  // namespace esphome
