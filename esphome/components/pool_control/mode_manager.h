#pragma once

#include <cstdint>

namespace esphome {
namespace pool_control {

// Pool operating modes. The numeric value of the base modes doubles as the
// index used by the YAML scene scripts (scene_auto sets 0, scene_spa 1, ...,
// scene_pool 6), so DO NOT reorder these without updating those scripts.
// AUTO and POOL share the same idle-pool hydraulics; AUTO is the only mode that
// also runs the scheduled cleaning rotation (POOL is a static hold). The three
// override modes live above the user scenes and are never user-selected.
enum class PoolMode : uint8_t {
  // Automatic program: idle-pool hydraulics, and the only base mode that also
  // runs the scheduled cleaning rotation (see mode_allows_cleaning). This is the
  // default base mode, so it holds value 0 (the fresh/persisted default).
  AUTO = 0,
  SPA = 1,
  SPILLOVER = 2,
  FOUNTAINS_LOW = 3,
  FOUNTAINS_HIGH = 4,
  SPA_BUBBLES = 5,
  // Static hold: identical idle-pool hydraulics to AUTO, but the scheduled
  // cleaning rotation does NOT run.
  POOL = 6,
  // --- override modes (arbitrated, not user-selectable) ---
  CLEANING = 7,
  FREEZE = 8,
  // Manual maintenance override. Sits above everything else: it suspends the
  // automation (freeze/cleaning/holding) so a technician has full manual
  // control, and it is the only way to change plumbing while a freeze holds.
  SERVICE = 9,
};

// Count of user-selectable base modes (AUTO..POOL, values 0..6).
static constexpr uint8_t NUM_BASE_MODES = 7;

inline const char *pool_mode_name(PoolMode m) {
  switch (m) {
    case PoolMode::POOL:
      return "Pool";
    case PoolMode::AUTO:
      return "Auto";
    case PoolMode::SPA:
      return "Spa";
    case PoolMode::SPILLOVER:
      return "Spillover";
    case PoolMode::FOUNTAINS_LOW:
      return "Fountains Low";
    case PoolMode::FOUNTAINS_HIGH:
      return "Fountains High";
    case PoolMode::SPA_BUBBLES:
      return "Spa Bubbles";
    case PoolMode::CLEANING:
      return "Cleaning";
    case PoolMode::FREEZE:
      return "Freeze Protect";
    case PoolMode::SERVICE:
      return "Service";
  }
  return "Unknown";
}

// Pure arbitration of the pool's operating mode. No framework dependencies, so
// it is exercised directly by host unit tests.
//
// Priority (highest wins):
//   FREEZE (safety, forces circulation) > CLEANING (scheduled) > base scene.
//
// A scheduled cleaning cycle only starts if the base scene permits it: the
// fountains scenes divert the return away from the filter/cleaner path, so
// cleaning is suppressed while they are selected.
class ModeManager {
 public:
  // Records user intent. Silently ignores the override modes (CLEANING/FREEZE)
  // and any out-of-range value, leaving the previous base mode untouched.
  void set_base_mode(PoolMode m) {
    if (static_cast<uint8_t>(m) < NUM_BASE_MODES)
      base_mode_ = m;
  }
  PoolMode base_mode() const { return base_mode_; }

  void set_freeze(bool on) { freeze_ = on; }
  bool freeze_active() const { return freeze_; }

  void set_cleaning_active(bool on) { cleaning_active_ = on; }
  bool cleaning_active() const { return cleaning_active_; }

  // Manual maintenance override (technician has full manual control).
  void set_service(bool on) { service_ = on; }
  bool service_active() const { return service_; }

  // Scheduled cleaning belongs to the automatic ("Auto") program only. The
  // static Pool hold and every manual scene override (spa, spillover,
  // fountains, bubbles) pause the schedule until the user returns to Auto.
  static bool mode_allows_cleaning(PoolMode m) { return m == PoolMode::AUTO; }

  // POOL and AUTO share the idle-pool hydraulics (skimmer/drain suction, pool
  // return, holding flow). Callers that key off "are we in the fixed idle-pool
  // plumbing" (holding suction rotation, filter-load / demux sensors) should use
  // this rather than comparing to POOL alone, so AUTO behaves like Pool between
  // cleaning cycles.
  static bool mode_is_pool_idle(PoolMode m) {
    return m == PoolMode::POOL || m == PoolMode::AUTO;
  }
  bool effective_is_pool_idle() const {
    return mode_is_pool_idle(effective_mode());
  }

  // Gate the scheduler checks before starting a cleaning cycle. Suspended
  // while a technician is in service mode or a freeze is protecting the lines.
  bool may_clean() const {
    return !service_ && !freeze_ && mode_allows_cleaning(base_mode_);
  }

  // True when the user may re-plumb to a base scene. Blocked while a freeze
  // holds (the lines must keep circulating) unless service mode is engaged.
  bool may_change_mode() const { return service_ || !freeze_; }

  // Arbitrated mode after applying the override priority.
  PoolMode effective_mode() const {
    if (service_)
      return PoolMode::SERVICE;
    if (freeze_)
      return PoolMode::FREEZE;
    if (cleaning_active_)
      return PoolMode::CLEANING;
    return base_mode_;
  }

 protected:
  PoolMode base_mode_{PoolMode::AUTO};
  bool freeze_{false};
  bool cleaning_active_{false};
  bool service_{false};
};

// Policy: which overrides stand the CHLORINE automation (dosing + generation)
// down. Only SERVICE today — a technician taking manual control should not have
// the cell nudging output or generating underneath them. FREEZE and CLEANING
// keep dosing on proven flow (harmless; the water is already circulating). This
// is the SINGLE definition of the policy; both the dosing gate (chlorine
// scripts) and the generation gate (cell relay) consult it so they can't drift.
// The freeze/cleaning params are carried (unused) so widening the policy later
// is a one-line change here.
inline bool chlorine_automation_suspended(bool service, bool /*freeze*/,
                                          bool /*cleaning*/) {
  return service;
}

}  // namespace pool_control
}  // namespace esphome
