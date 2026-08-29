#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/number/number.h"
#include "chlorine_demand.h"
#include "mode_manager.h"
#include "holding_pattern.h"
#include "freeze_controller.h"
#include "pump_interlock.h"
#include "cell_wear.h"
#include "suction_temp.h"
#include "status_policy.h"
#include "comms_watchdog.h"
#include "fc_target.h"
#include "filter_health.h"
#include "holding_flow.h"
#include "supervisor.h"
#include "cleaning_phase.h"
#include "iso8601.h"
#include "plumbing.h"
#include "plumbing_arbiter.h"

namespace esphome {
namespace pool_control {

// ESPHome glue around the chlorine mass balance: owns the demand observer,
// persists its learned state across reboots, and pushes the computed output to
// the chlorinator's output % number. Called from thin YAML lambdas.
class PoolControl : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // --- Wiring (set by codegen) ---
  void set_output_number(number::Number *n) { output_number_ = n; }

  // --- Actions callable from YAML ---
  // Home Assistant's `last_updated` arrives as an ISO-8601 STRING, so it must
  // come in via a text_sensor and be parsed here -- importing it as a numeric
  // sensor yields NaN and silently kills the demand observer. See iso8601.h.
  float parse_reading_ts(const std::string &s) const {
    return esphome::pool_control::iso8601_to_epoch2020(s.c_str());
  }
  // Boost owns the output while active.
  void set_boost_active(bool b) { boost_active_ = b; }
  bool boost_active() const { return boost_active_; }
  // Directly drive the output % (used by the boost script). Routed through here so
  // every write to the number goes through make_call().
  void set_output_direct(float value);

  // --- Mode arbitration (freeze > cleaning > base scene) ---
  // User picks a scene: record intent (persists across reboots).
  void set_base_mode(int m);
  int base_mode() const { return static_cast<int>(modes_.base_mode()); }
  // Freeze override (safety); not persisted — re-evaluated from live temp.
  void set_freeze(bool b) { modes_.set_freeze(b); }
  bool freeze_active() const { return modes_.freeze_active(); }
  // Marks a cleaning cycle in progress (for the effective-mode display).
  void set_cleaning_active(bool b) { modes_.set_cleaning_active(b); }
  bool cleaning_active() const { return modes_.cleaning_active(); }
  // Service (manual maintenance) override; highest priority, not persisted.
  void set_service(bool b) { modes_.set_service(b); }
  bool service_active() const { return modes_.service_active(); }
  // Scheduler gate: may a cleaning cycle start right now?
  bool may_clean() const { return modes_.may_clean(); }
  // May the user re-plumb to a base scene right now? Blocked during freeze
  // unless service mode is engaged.
  bool may_change_mode() const { return modes_.may_change_mode(); }
  // Arbitrated mode after overrides.
  int effective_mode() const { return static_cast<int>(modes_.effective_mode()); }
  const char *effective_mode_name() const { return pool_mode_name(modes_.effective_mode()); }
  const char *base_mode_name() const { return pool_mode_name(modes_.base_mode()); }
  // True when the arbitrated mode uses the idle-pool hydraulics (POOL or AUTO):
  // holding suction rotation + fixed-hydraulics sensors apply in both.
  bool effective_is_pool_idle() const { return modes_.effective_is_pool_idle(); }

  // Scene diverter/blower/flow truth table (host-tested); the thin apply_scene
  // script maps flow selector -> the live GPM number.
  ScenePlumbing scene_plumbing(int mode) const {
    return esphome::pool_control::scene_plumbing(mode);
  }

  // --- Holding pattern ---
  // Should idle circulation pull from the main drain right now (else skimmer)?
  bool holding_drain_now(int sec_of_day, int period_s, int drain_s) const {
    return esphome::pool_control::holding_drain_now(sec_of_day, period_s, drain_s);
  }
  // Pick the holding speed setpoint matching the live suction path.
  float holding_rpm_for_suction(float rpm_skimmer, float rpm_drain,
                                bool on_drain) const {
    return esphome::pool_control::holding_rpm_for_suction(rpm_skimmer, rpm_drain,
                                                          on_drain);
  }

  // --- Freeze protection ---
  // One freeze tick: hysteresis + loop-rotation decision. Caller handles the
  // service-mode override and NaN temperature (both mean "skip this tick").
  FreezeDecision freeze_tick(float temp_c, float threshold_c, float hysteresis_c,
                             int sec_of_day, int rotate_s, float normal_flow,
                             uint32_t active_elapsed_ms = 0,
                             uint32_t min_runtime_ms = 0) const {
    return esphome::pool_control::freeze_tick(temp_c, threshold_c, hysteresis_c,
                                              modes_.freeze_active(), sec_of_day,
                                              rotate_s, normal_flow,
                                              active_elapsed_ms, min_runtime_ms);
  }

  // Fail-safe freeze variant: the outdoor probe is unreadable/stale, so protect
  // anyway (caller decides when via temp_sensor_stale + raises the alarm).
  FreezeDecision freeze_failsafe(float threshold_c, float hysteresis_c,
                                 int sec_of_day, int rotate_s, float normal_flow) const {
    return esphome::pool_control::freeze_failsafe(threshold_c, hysteresis_c,
                                                 modes_.freeze_active(), sec_of_day,
                                                 rotate_s, normal_flow);
  }
  // Has a periodically-updated temperature source gone stale (NaN readings stop
  // refreshing last_seen)? Lets the freeze lambda fail safe.
  bool temp_sensor_stale(uint32_t now_ms, uint32_t last_seen_ms,
                         uint32_t max_stale_ms) const {
    return esphome::pool_control::temp_sensor_stale(now_ms, last_seen_ms, max_stale_ms);
  }

  // --- Pump / valve-travel interlock ---
  // Clamp the pump to the valve-change creep while a diverter is travelling.
  float effective_pump_flow(float desired, float creep, uint32_t now_ms,
                            uint32_t motion_until_ms) const {
    return esphome::pool_control::effective_pump_flow(desired, creep, now_ms,
                                                      motion_until_ms);
  }
  // Creep dip, full stop, or leave alone for the move that just happened.
  TravelAction valve_travel_action(bool stop_valve, bool pump_powered, bool speed_mode,
                                   float desired_flow, float creep, float travel_s) const {
    return esphome::pool_control::valve_travel_action(stop_valve, pump_powered, speed_mode,
                                                      desired_flow, creep, travel_s);
  }
  // Is the open travel window one that demands zero flow?
  bool travel_stop_active(bool stop_window, uint32_t now_ms,
                          uint32_t motion_until_ms) const {
    return esphome::pool_control::travel_stop_active(stop_window, now_ms,
                                                     motion_until_ms);
  }
  // Encode a flow setpoint (GPM) as the Pentair single-byte command.
  int flow_to_cmd(float flow_gpm) const {
    return esphome::pool_control::flow_to_cmd(flow_gpm);
  }
  // Encode a speed setpoint (RPM) as the Pentair VSF speed command.
  int rpm_to_cmd(float rpm) const {
    return esphome::pool_control::rpm_to_cmd(rpm);
  }

  // --- Cell runtime ---
  CellWearStep cell_wear_step(float pct, float dt_seconds) const {
    return esphome::pool_control::cell_wear_step(pct, dt_seconds);
  }

  // --- Chlorine demand (observation only; drives nothing yet) ---
  // `ts` is the parsed reading timestamp in epoch-2020 SECONDS, as returned by
  // parse_reading_ts; the observer works in days.
  void observe_demand(float ts, float fc, float equiv_hours, float ppm_per_equiv_h);
  float chlorine_demand() const { return demand_.demand(); }
  float ppm_per_equiv_hour(float cell_lb_per_day, float pool_gallons) const {
    return esphome::pool_control::ppm_per_equiv_hour(cell_lb_per_day, pool_gallons);
  }
  float output_for_demand(float demand, float ppm_per_equiv_h,
                          float generating_hours_per_day) const {
    return esphome::pool_control::output_for_demand(demand, ppm_per_equiv_h,
                                                    generating_hours_per_day);
  }
  // What mass-balance control WOULD command right now. NaN until demand is
  // known, so it doubles as the "is the estimate ready" test.
  float mass_balance_pct(float fc, float target, float tau_days,
                         float ppm_per_equiv_h, float generating_hours_per_day,
                         float floor_pct, float ceil_pct) const {
    return esphome::pool_control::mass_balance_output(
               demand_.demand(), fc, target, tau_days, ppm_per_equiv_h,
               generating_hours_per_day, floor_pct, ceil_pct)
        .pct;
  }
  // Compute and push it. Returns false (leaving the setpoint alone) under boost
  // or alarm, or before demand is known.
  bool apply_mass_balance(float fc, float target, bool alarm, float tau_days,
                          float ppm_per_equiv_h, float generating_hours_per_day,
                          float floor_pct, float ceil_pct);

  // --- Suction-source temperature demux ---
  int suction_source(bool valve1_spa, bool valve2_drain) const {
    return esphome::pool_control::suction_source(valve1_spa, valve2_drain);
  }
  bool suction_settled(uint32_t now_ms, uint32_t changed_ms,
                       uint32_t settle_ms) const {
    return esphome::pool_control::suction_settled(now_ms, changed_ms, settle_ms);
  }
  uint32_t settle_ms_for_gallons(float gallons, float flow_gpm) const {
    return esphome::pool_control::settle_ms_for_gallons(gallons, flow_gpm);
  }

  // --- Status LED precedence ---
  int status_state(bool service, bool comms, bool alarm, bool freeze,
                   bool cleaning, bool boost, bool warn) const {
    return esphome::pool_control::status_state(service, comms, alarm, freeze,
                                               cleaning, boost, warn);
  }

  // --- RS485 comms watchdog ---
  bool comms_lost(uint32_t now_ms, uint32_t powered_since_ms,
                  uint32_t last_seen_ms, uint32_t grace_ms,
                  uint32_t timeout_ms) const {
    return esphome::pool_control::comms_lost(now_ms, powered_since_ms,
                                             last_seen_ms, grace_ms, timeout_ms);
  }

  // --- Pool chemistry ---
  float fc_target_from_cya(float cya, float ratio, float floor_ppm,
                           float ceil_ppm) const {
    return esphome::pool_control::fc_target_from_cya(cya, ratio, floor_ppm,
                                                     ceil_ppm);
  }

  // --- Filter health ---
  float filter_load_pct(float rpm, float flow, float base_rpm,
                        float base_flow) const {
    return esphome::pool_control::filter_load_pct(rpm, flow, base_rpm, base_flow);
  }
  float filter_dirt_pct(float load_pct, float clean_threshold_pct) const {
    return esphome::pool_control::filter_dirt_pct(load_pct, clean_threshold_pct);
  }
  bool filter_clean_needed(float load_pct, float threshold_pct) const {
    return esphome::pool_control::filter_clean_needed(load_pct, threshold_pct);
  }
  bool filter_reference_plumbing(bool spa_suction, bool main_drain_suction,
                                 bool spa_return, bool fountain_return) const {
    return esphome::pool_control::filter_reference_plumbing(
        spa_suction, main_drain_suction, spa_return, fountain_return);
  }
  uint16_t filter_steady_count(float rpm, float prev_rpm, uint16_t count,
                               float tol_rpm) const {
    return esphome::pool_control::filter_steady_count(rpm, prev_rpm, count,
                                                      tol_rpm);
  }
  bool filter_capture_ready(bool armed, bool pump_running, bool pool_idle,
                            bool reference_plumbing, bool loop_settled,
                            float rpm, float flow, uint16_t steady_count,
                            uint16_t required_count) const {
    return esphome::pool_control::filter_capture_ready(
        armed, pump_running, pool_idle, reference_plumbing, loop_settled, rpm,
        flow, steady_count, required_count);
  }

  // Single definition of the chlorine-suspension policy (service today). Used by
  // both the dosing scripts and the cell-generation gate so they can't diverge.
  bool chlorine_suspended() const {
    return esphome::pool_control::chlorine_automation_suspended(
        modes_.service_active(), modes_.freeze_active(), modes_.cleaning_active());
  }

  // --- Closed-loop holding flow below the drive's 20 GPM floor ---
  // Unlike the other cores this one is STATEFUL (it owns a small state
  // machine), so it lives as a member rather than a free function. See
  // holding_flow.h for why the flow switch's 14:1 open/reclose asymmetry
  // forces this shape.
  HoldingFlowController::Out holding_tick(uint32_t now_ms, bool pool_idle,
                                          bool pump_running, float target_flow,
                                          float flow_avg, float rpm_now,
                                          bool no_flow, uint16_t steady_count) {
    return hold_.tick(now_ms, pool_idle, pump_running, target_flow, flow_avg,
                      rpm_now, no_flow, steady_count);
  }
  bool holding_faulted() const { return hold_.faulted(); }
  uint8_t holding_trips() const { return hold_.trips(); }
  bool holding_settled_for_baseline() const {
    return hold_.settled_for_baseline();
  }
  void holding_reset_fault() { hold_.reset_fault(); }

  // --- Always-on supervisor (booster guard + status LED) ---
  SupervisorOutputs supervise(bool pump_running, bool pump_priming,
                              bool pump_alarm, bool service, bool freeze,
                              bool cleaning, bool boost, bool comms_lost,
                              bool alarm, bool warn) const {
    return esphome::pool_control::supervise(SupervisorInputs{
        pump_running, pump_priming, pump_alarm, service, freeze, cleaning,
        boost, comms_lost, alarm, warn});
  }

  // --- Cleaning cycle: elapsed time -> phase + booster permit (deadline math) ---
  CleanTickOut cleaning_tick(uint32_t elapsed_ms, uint32_t spa_ms,
                             uint32_t spillover_ms, uint32_t scrub_ms,
                             uint32_t prime_ms, bool freeze) const {
    return esphome::pool_control::cleaning_tick(
        elapsed_ms,
        CleanDurations{spa_ms, spillover_ms, scrub_ms, prime_ms},
        freeze);
  }

  // Absolute diverter/blower/flow pattern for a cleaning phase (host-tested);
  // shares the ScenePlumbing shape + FlowSel mapping used by scene_plumbing.
  ScenePlumbing cleaning_plumbing(int phase) const {
    return esphome::pool_control::cleaning_plumbing(
        static_cast<CleanPhase>(phase));
  }

  // Single plumbing arbiter: who owns the diverters/pump now + what pattern.
  // Priority SERVICE > FREEZE > CLEANING > SCENE (see plumbing_arbiter.h).
  PlumbingResolution resolve_plumbing(int base_mode, bool service,
                                      bool freeze_active, bool cleaning_active,
                                      int clean_phase, int sec_of_day,
                                      int rotate_s, float normal_flow) const {
    return esphome::pool_control::resolve_plumbing(PlumbingInputs{
        base_mode, service, freeze_active, cleaning_active, clean_phase,
        sec_of_day, rotate_s, normal_flow});
  }

 protected:
  void save_mode_();
  void save_demand_();

  number::Number *output_number_{nullptr};
  bool boost_active_{false};
  DemandObserver demand_;
  HoldingFlowController hold_;
  ModeManager modes_;
  ESPPreferenceObject mode_pref_;
  ESPPreferenceObject demand_pref_;
};

}  // namespace pool_control
}  // namespace esphome
