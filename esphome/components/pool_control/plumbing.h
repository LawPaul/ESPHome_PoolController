#pragma once

#include <cstdint>
#include "mode_manager.h"

namespace esphome {
namespace pool_control {

// Which pump-flow setpoint a scene wants. The actual GPM values live in YAML
// number entities, so the pure core returns a selector that the thin actuation
// layer maps to the live setpoint. Keeps framework/number lookups out of core.
enum class FlowSel : uint8_t {
  HOLDING = 0,        // idle holding-pattern flow
  NORMAL = 1,         // standard scene flow (spa / spillover)
  BUBBLES = 2,        // spa + air blower, livelier flow
  FOUNTAIN_LOW = 3,   // fountains, low flow
  FOUNTAIN_HIGH = 4,  // fountains, high flow
  CLEAN = 5,          // cleaning-cycle flow (spa filter / spillover phases)
  BOOSTER = 6,        // scrub phase: main-pump feed for the Polaris booster
};

// Valve + blower + flow pattern for a user-selectable base scene. This is the
// scene truth table, lifted out of the six near-identical scene_* scripts so
// the diverter routing is one host-tested source of truth instead of being
// duplicated (and drifting) across YAML.
//
// JVA valve convention (matches apply_plumbing / the scene comments):
//   valve1_spa      false = pool suction     true = spa suction
//   valve2_drain    false = skimmers         true = main drain (pool-suction sub-select)
//   valve3_spa      false = pool return      true = spa return
//   valve4_fountain false = jets             true = fountains  (pool-return sub-select)
struct ScenePlumbing {
  bool valve1_spa;
  bool valve2_drain;
  bool valve3_spa;
  bool valve4_fountain;
  bool blower;
  FlowSel flow;
};

// Map a base mode (PoolMode 0..6) to its diverter/blower/flow pattern. Unknown
// or override modes (CLEANING/FREEZE/SERVICE) fall back to the idle POOL scene,
// which is the safe default (low holding flow, everything pointed at the pool).
// AUTO shares POOL's idle-pool pattern; the two differ only in whether the
// scheduled cleaning rotation may run (see ModeManager::mode_allows_cleaning).
inline ScenePlumbing scene_plumbing(int mode) {
  switch (static_cast<PoolMode>(mode)) {
    case PoolMode::SPA:  // spa loop, standard flow
      return {true, false, true, false, false, FlowSel::NORMAL};
    case PoolMode::SPILLOVER:  // pool suction, spa return -> spills back to pool
      return {false, false, true, false, false, FlowSel::NORMAL};
    case PoolMode::FOUNTAINS_LOW:  // pool return via fountains, low flow
      return {false, false, false, true, false, FlowSel::FOUNTAIN_LOW};
    case PoolMode::FOUNTAINS_HIGH:  // pool return via fountains, high flow
      return {false, false, false, true, false, FlowSel::FOUNTAIN_HIGH};
    case PoolMode::SPA_BUBBLES:  // spa loop + air blower
      return {true, false, true, false, true, FlowSel::BUBBLES};
    case PoolMode::POOL:  // idle holding pattern (skimmer suction, pool return)
    case PoolMode::AUTO:  // automatic program: same idle-pool hydraulics as POOL
    default:
      return {false, false, false, false, false, FlowSel::HOLDING};
  }
}

}  // namespace pool_control
}  // namespace esphome
