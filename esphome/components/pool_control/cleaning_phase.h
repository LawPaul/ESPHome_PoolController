#pragma once
// Deadline-math for the intelligent cleaning cycle.
//
// The cleaning cycle is a timed choreography of four phases. Historically the
// timing lived in a `mode: restart` YAML script built from sequential `delay:`
// actions, which made the phase boundaries and the booster prime-window
// impossible to unit-test. This core turns "how long has the cycle been
// running?" into "which phase are we in, and may the booster run?" as a pure
// function of elapsed time — so the imperative shell only has to actuate.
//
// Phases (in order):
//   1 SPA        filter the spa
//   2 SPILLOVER  refresh the spa with pool water (spillover)
//   3 SCRUB      pool scrub with the Polaris on main-drain suction
//   4 DONE       cycle complete — hand back to the base mode
#include <cstdint>
#include "plumbing.h"

namespace esphome {
namespace pool_control {

enum CleanPhase : int {
  CLEAN_IDLE = 0,       // cycle not running
  CLEAN_SPA = 1,        // phase 1: filter the spa
  CLEAN_SPILLOVER = 2,  // phase 2: spillover refresh
  CLEAN_SCRUB = 3,      // phase 3: pool scrub (Polaris)
  CLEAN_DONE = 4,       // cycle finished
};

struct CleanDurations {
  uint32_t spa_ms;        // phase 1 length
  uint32_t spillover_ms;  // phase 2 length
  uint32_t scrub_ms;      // phase 3 length
  uint32_t prime_ms;      // flow-establish window before the booster in scrub
};

struct CleanTickOut {
  CleanPhase phase;      // phase for this elapsed time
  bool booster_permit;   // Polaris may run (mid-scrub, after prime, freeze off)
};

// Pure: elapsed time since the cycle started -> current phase + booster permit.
// Fail-safe by construction: the booster is only ever permitted inside the
// scrub phase, only after the prime window has fully elapsed, and never while a
// freeze is active. Any time past the last phase boundary reports CLEAN_DONE.
inline CleanTickOut cleaning_tick(uint32_t elapsed_ms, const CleanDurations &d,
                                  bool freeze) {
  const uint32_t end_spa = d.spa_ms;
  const uint32_t end_spillover = end_spa + d.spillover_ms;
  const uint32_t end_scrub = end_spillover + d.scrub_ms;

  CleanTickOut out{CLEAN_DONE, false};
  if (elapsed_ms < end_spa) {
    out.phase = CLEAN_SPA;
  } else if (elapsed_ms < end_spillover) {
    out.phase = CLEAN_SPILLOVER;
  } else if (elapsed_ms < end_scrub) {
    out.phase = CLEAN_SCRUB;
    const uint32_t into_scrub = elapsed_ms - end_spillover;
    out.booster_permit = !freeze && into_scrub >= d.prime_ms;
  } else {
    out.phase = CLEAN_DONE;
  }
  return out;
}

// Absolute diverter/blower/flow pattern for a cleaning phase, in the same shape
// as the scene truth table (plumbing.h). Unlike the old edge-triggered blocks
// that left some valves to carry over from the previous phase, this fully
// specifies all four diverters per phase, so the cleaning choreography is a
// tested table and no longer depends on implicit prior state. Relays already in
// position simply don't move (turn_on/turn_off on an unchanged relay is a
// no-op), so this introduces no extra actuation in the common path.
//   Phase 1 SPA        spa suction + spa return, clean flow
//   Phase 2 SPILLOVER  pool suction (skimmer) + spa return -> spills to pool
//   Phase 3 SCRUB      pool suction (main drain) + pool return, booster feed flow
// IDLE/DONE fall back to the safe idle pattern (everything at the pool).
inline ScenePlumbing cleaning_plumbing(CleanPhase phase) {
  switch (phase) {
    case CLEAN_SPA:
      return {true, false, true, false, false, FlowSel::CLEAN};
    case CLEAN_SPILLOVER:
      return {false, false, true, false, false, FlowSel::CLEAN};
    case CLEAN_SCRUB:
      return {false, true, false, false, false, FlowSel::BOOSTER};
    case CLEAN_IDLE:
    case CLEAN_DONE:
    default:
      return {false, false, false, false, false, FlowSel::HOLDING};
  }
}

}  // namespace pool_control
}  // namespace esphome
