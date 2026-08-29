// Host unit tests for the pure ModeManager arbitration core.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_mode_manager.cpp -o /tmp/mmtest && /tmp/mmtest
#include "../components/pool_control/mode_manager.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

using esphome::pool_control::ModeManager;
using esphome::pool_control::PoolMode;
using esphome::pool_control::pool_mode_name;

int main() {
  // Defaults: Auto base + effective (the automatic program), cleaning allowed.
  {
    ModeManager m;
    assert(m.base_mode() == PoolMode::AUTO);
    assert(m.effective_mode() == PoolMode::AUTO);
    assert(m.may_clean());
    assert(!m.freeze_active());
    assert(!m.cleaning_active());
  }
  // Only Auto permits scheduled cleaning; the static Pool hold and every manual
  // scene override pause the schedule.
  {
    ModeManager m;
    m.set_base_mode(PoolMode::AUTO);
    assert(m.may_clean());
    for (auto md : {PoolMode::POOL, PoolMode::SPA, PoolMode::SPILLOVER,
                    PoolMode::FOUNTAINS_LOW, PoolMode::FOUNTAINS_HIGH,
                    PoolMode::SPA_BUBBLES}) {
      m.set_base_mode(md);
      assert(m.base_mode() == md);
      assert(m.effective_mode() == md);
      assert(!m.may_clean());
    }
    assert(ModeManager::mode_allows_cleaning(PoolMode::AUTO));
    assert(!ModeManager::mode_allows_cleaning(PoolMode::POOL));
    assert(!ModeManager::mode_allows_cleaning(PoolMode::SPA));
    assert(!ModeManager::mode_allows_cleaning(PoolMode::FOUNTAINS_HIGH));
  }
  // Freeze overrides any base scene and blocks cleaning (safety).
  {
    ModeManager m;
    m.set_base_mode(PoolMode::SPA);
    m.set_freeze(true);
    assert(m.effective_mode() == PoolMode::FREEZE);
    assert(!m.may_clean());
    m.set_base_mode(PoolMode::AUTO);  // still frozen
    assert(m.effective_mode() == PoolMode::FREEZE);
    assert(!m.may_clean());
    // Base intent is preserved underneath the override.
    assert(m.base_mode() == PoolMode::AUTO);
    m.set_freeze(false);
    assert(m.effective_mode() == PoolMode::AUTO);
    assert(m.may_clean());
  }
  // Cleaning-in-progress shows as CLEANING, but freeze still wins.
  {
    ModeManager m;
    m.set_base_mode(PoolMode::AUTO);
    m.set_cleaning_active(true);
    assert(m.effective_mode() == PoolMode::CLEANING);
    m.set_freeze(true);
    assert(m.effective_mode() == PoolMode::FREEZE);  // freeze > cleaning
    m.set_freeze(false);
    assert(m.effective_mode() == PoolMode::CLEANING);
    m.set_cleaning_active(false);
    assert(m.effective_mode() == PoolMode::AUTO);
  }
  // set_base_mode rejects override modes and out-of-range values.
  {
    ModeManager m;
    m.set_base_mode(PoolMode::SPA);
    m.set_base_mode(PoolMode::CLEANING);  // ignored
    assert(m.base_mode() == PoolMode::SPA);
    m.set_base_mode(PoolMode::FREEZE);  // ignored
    assert(m.base_mode() == PoolMode::SPA);
    m.set_base_mode(static_cast<PoolMode>(99));  // ignored
    assert(m.base_mode() == PoolMode::SPA);
  }
  // Mode names.
  {
    assert(std::strcmp(pool_mode_name(PoolMode::POOL), "Pool") == 0);
    assert(std::strcmp(pool_mode_name(PoolMode::AUTO), "Auto") == 0);
    assert(std::strcmp(pool_mode_name(PoolMode::FOUNTAINS_HIGH), "Fountains High") == 0);
    assert(std::strcmp(pool_mode_name(PoolMode::FREEZE), "Freeze Protect") == 0);
    assert(std::strcmp(pool_mode_name(PoolMode::CLEANING), "Cleaning") == 0);
    assert(std::strcmp(pool_mode_name(PoolMode::SERVICE), "Service") == 0);
    assert(std::strcmp(pool_mode_name(PoolMode::SPA_BUBBLES), "Spa Bubbles") == 0);
  }

  // POOL and AUTO share the idle-pool hydraulics; manual scenes and the
  // overrides do not. AUTO is the automatic program (adds cleaning) but is
  // hydraulically Pool whenever a cycle isn't running.
  {
    assert(ModeManager::mode_is_pool_idle(PoolMode::POOL));
    assert(ModeManager::mode_is_pool_idle(PoolMode::AUTO));
    for (auto md : {PoolMode::SPA, PoolMode::SPILLOVER, PoolMode::FOUNTAINS_LOW,
                    PoolMode::FOUNTAINS_HIGH, PoolMode::SPA_BUBBLES,
                    PoolMode::CLEANING, PoolMode::FREEZE, PoolMode::SERVICE})
      assert(!ModeManager::mode_is_pool_idle(md));

    ModeManager m;
    m.set_base_mode(PoolMode::AUTO);
    assert(m.effective_is_pool_idle());   // idle Auto == pool hydraulics
    m.set_cleaning_active(true);
    assert(!m.effective_is_pool_idle());  // cleaning owns different plumbing
    m.set_cleaning_active(false);
    m.set_base_mode(PoolMode::POOL);
    assert(m.effective_is_pool_idle());   // static Pool hold
    m.set_base_mode(PoolMode::SPA);
    assert(!m.effective_is_pool_idle());
  }

  // Spa Bubbles is a selectable base mode (index 5); overrides above it stay
  // rejected by set_base_mode.
  {
    ModeManager m;
    m.set_base_mode(PoolMode::SPA_BUBBLES);
    assert(m.base_mode() == PoolMode::SPA_BUBBLES);
    assert(m.effective_mode() == PoolMode::SPA_BUBBLES);
    m.set_base_mode(PoolMode::CLEANING);   // override: ignored
    assert(m.base_mode() == PoolMode::SPA_BUBBLES);
  }

  // Freeze locks out base-mode changes; service mode is the escape hatch.
  {
    ModeManager m;
    assert(m.may_change_mode());          // idle: free to switch
    m.set_freeze(true);
    assert(!m.may_change_mode());         // frozen: locked
    assert(!m.may_clean());               // and no cleaning
    m.set_service(true);
    assert(m.may_change_mode());          // service overrides the lock
  }

  // Service mode is top priority and suspends cleaning.
  {
    ModeManager m;
    m.set_base_mode(PoolMode::SPA);
    m.set_cleaning_active(true);
    m.set_freeze(true);
    assert(m.effective_mode() == PoolMode::FREEZE);   // freeze > cleaning
    m.set_service(true);
    assert(m.effective_mode() == PoolMode::SERVICE);  // service > freeze
    assert(!m.may_clean());                           // no auto-clean in service
    m.set_service(false);
    assert(m.effective_mode() == PoolMode::FREEZE);   // back to freeze
  }

  // Chlorine automation-suspension policy: ONLY service stands dosing +
  // generation down; freeze and cleaning keep dosing on proven flow.
  {
    using esphome::pool_control::chlorine_automation_suspended;
    assert(!chlorine_automation_suspended(false, false, false));  // normal: dose
    assert(chlorine_automation_suspended(true, false, false));    // service: stood down
    assert(!chlorine_automation_suspended(false, true, false));   // freeze: keeps dosing
    assert(!chlorine_automation_suspended(false, false, true));   // cleaning: keeps dosing
    assert(chlorine_automation_suspended(true, true, true));      // service dominates
  }

  std::printf("All mode manager tests passed.\n");
  return 0;
}
