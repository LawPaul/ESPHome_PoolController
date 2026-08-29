#pragma once

#include <cstdint>
#include "plumbing.h"
#include "cleaning_phase.h"
#include "freeze_controller.h"

namespace esphome {
namespace pool_control {

// =============================================================================
// Plumbing arbiter — the single, host-tested authority for WHO owns the
// diverters/pump right now and WHAT pattern to apply. It composes the tested
// pattern tables (scene_plumbing, cleaning_plumbing) and the freeze rotation
// (freeze_plumbing) under one explicit priority, replacing the implicit
// "whichever interval wrote the valves last" ordering that used to be smeared
// across the freeze interval, clean_tick and the scene scripts.
//
// Priority (highest wins):
//   SERVICE  — manual maintenance: automation is hands-off, valves untouched.
//   FREEZE   — safety override: forced circulation + loop rotation.
//   CLEANING — scheduled cycle owns the diverters while it runs.
//   SCENE    — the user's selected base mode (idle holding rotates its own
//              suction drain independently).
// SERVICE sits above FREEZE defensively; in practice service suspends freeze so
// freeze_active is already false, but "hands-off" must win regardless.
// =============================================================================

enum class PlumbingOwner : uint8_t {
  SERVICE = 0,
  FREEZE = 1,
  CLEANING = 2,
  SCENE = 3,
};

struct PlumbingInputs {
  int base_mode;        // user scene 0..5 (PoolMode)
  bool service;         // manual maintenance override
  bool freeze_active;   // freeze protection engaged
  bool cleaning_active; // a cleaning cycle is running
  int clean_phase;      // current CleanPhase (only read when cleaning_active)
  int sec_of_day;       // for the freeze rotation phase
  int rotate_s;         // freeze rotation period (s)
  float normal_flow;    // steady-state flow (freeze forced-flow floor input)
};

struct PlumbingResolution {
  PlumbingOwner owner;
  bool actuate;          // false only under SERVICE (leave everything as-is)
  bool valve1_spa;
  bool valve2_drain;
  bool valve3_spa;
  bool valve4_fountain;
  bool blower;
  bool forced_flow;      // true => command forced_gpm; false => resolve `flow`
  float forced_gpm;      // valid iff forced_flow (freeze)
  FlowSel flow;          // valid iff !forced_flow (scene / cleaning)
};

// Pure arbitration. Copies the winning owner's pattern into the resolution so
// the imperative shell only has to actuate what it's told (or nothing, under
// SERVICE).
inline PlumbingResolution resolve_plumbing(const PlumbingInputs &in) {
  PlumbingResolution r{};

  // SERVICE: technician owns the equipment; never move valves automatically.
  if (in.service) {
    r.owner = PlumbingOwner::SERVICE;
    r.actuate = false;
    return r;
  }
  r.actuate = true;

  // FREEZE: forced circulation + loop rotation; fountains + blower held off.
  if (in.freeze_active) {
    FreezeDecision f = freeze_plumbing(in.sec_of_day, in.rotate_s, in.normal_flow);
    r.owner = PlumbingOwner::FREEZE;
    r.valve1_spa = f.valve1_spa;
    r.valve2_drain = f.valve2_drain;
    r.valve3_spa = f.valve3_spa;
    r.valve4_fountain = false;
    r.blower = false;
    r.forced_flow = true;
    r.forced_gpm = f.forced_flow;
    return r;
  }

  // CLEANING: the scheduled cycle's per-phase pattern.
  if (in.cleaning_active) {
    ScenePlumbing p = cleaning_plumbing(static_cast<CleanPhase>(in.clean_phase));
    r.owner = PlumbingOwner::CLEANING;
    r.valve1_spa = p.valve1_spa;
    r.valve2_drain = p.valve2_drain;
    r.valve3_spa = p.valve3_spa;
    r.valve4_fountain = p.valve4_fountain;
    r.blower = p.blower;
    r.forced_flow = false;
    r.flow = p.flow;
    return r;
  }

  // SCENE: the user's base mode. valve2 (suction drain) is left to the base
  // pattern here; the holding-pattern interval rotates it during idle POOL.
  ScenePlumbing p = scene_plumbing(in.base_mode);
  r.owner = PlumbingOwner::SCENE;
  r.valve1_spa = p.valve1_spa;
  r.valve2_drain = p.valve2_drain;
  r.valve3_spa = p.valve3_spa;
  r.valve4_fountain = p.valve4_fountain;
  r.blower = p.blower;
  r.forced_flow = false;
  r.flow = p.flow;
  return r;
}

}  // namespace pool_control
}  // namespace esphome
