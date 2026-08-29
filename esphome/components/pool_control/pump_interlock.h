#pragma once

#include <cmath>
#include <cstdint>

namespace esphome {
namespace pool_control {

// Pure pump/salt-cell safety decisions. Framework-free so the clamp math and
// the flow-interlock truth table are exercised by host unit tests; the YAML
// lambdas only read sensor state and actuate (commandFlow / relay writes).

// Effective pump flow while a diverter may be travelling. Clamp DOWN to the low
// valve-change creep until the motion window (millis deadline) expires, so a
// flow command issued mid-travel can never spin the pump up against a transiting
// valve (gear wear / pressure surge / momentary dead-head). Only ever reduces
// flow — never raises a requested creep.
//
// Note: `now_ms` and `motion_until_ms` are raw millis(); the comparison shares
// the same ~49-day wraparound behaviour as the caller's clock (acceptable: the
// window is seconds long and self-clears).
inline float effective_pump_flow(float desired_flow, float valve_creep_flow,
                                 uint32_t now_ms, uint32_t motion_until_ms) {
  if (now_ms < motion_until_ms && desired_flow > valve_creep_flow)
    return valve_creep_flow;
  return desired_flow;
}

// How the pump must behave for the travel window a diverter move just opened.
enum class TravelAction : uint8_t {
  RUN = 0,    // no restriction: command the requested setpoint
  CREEP = 1,  // dip to the valve-change creep flow
  STOP = 2,   // hold the drive stopped for the whole window
};

// Most diverters only need the creep dip: they are true 3-way valves that
// bypass mid-travel, so reduced flow is harmless. The fountain return is the
// exception -- at its partial positions the fountain feed is throttled enough
// that the arcing jets fall short and dribble onto the deck instead of into
// the pool, and the only fix is no flow at all. `stop_valve` marks that case.
//
// STOP applies in speed mode too: unlike the creep dip (skipped there because
// the holding rpm is already below the creep), "no flow" is expressible in
// either unit -- stopping the drive is a separate command from the setpoint.
inline TravelAction valve_travel_action(bool stop_valve, bool pump_powered,
                                        bool speed_mode, float desired_flow,
                                        float valve_creep_flow, float travel_s) {
  if (travel_s <= 0.0f || !pump_powered)
    return TravelAction::RUN;
  if (stop_valve)
    return TravelAction::STOP;
  if (speed_mode)
    return TravelAction::RUN;
  if (desired_flow > valve_creep_flow)
    return TravelAction::CREEP;
  return TravelAction::RUN;
}

// True while an open travel window demands zero flow. Scene actuation writes
// the valves first and the setpoint second, so a flow/speed command routinely
// lands mid-travel; this is what makes it hold the drive stopped rather than
// spin the pump straight back up against the valve the guard just stopped for.
// Shares effective_pump_flow's millis() wraparound note.
inline bool travel_stop_active(bool stop_window, uint32_t now_ms,
                               uint32_t motion_until_ms) {
  return stop_window && now_ms < motion_until_ms;
}

// Encode a flow setpoint as the Pentair single-byte flow command. The wire unit
// is native GPM (njSPC), so this is the identity — rounded to the nearest whole
// GPM and clamped to 0..255 (the single-byte ceiling is 255 GPM). One source of
// truth for the encoding the pump scripts all share.
inline int flow_to_cmd(float flow_gpm) {
  int cmd = (int) lroundf(flow_gpm);
  if (cmd < 0)
    cmd = 0;
  if (cmd > 255)
    cmd = 255;
  return cmd;
}

// Speed-mode setpoint range of the IntelliFlo VSF drive. Mirrors RPM_MIN /
// RPM_MAX in the pentair component's protocol.h (which clamps again on the
// wire); duplicated here so the pure core stays framework-free and host-
// testable. Keep the two in sync.
static const int RPM_CMD_MIN = 450;
static const int RPM_CMD_MAX = 3450;

// Encode a speed setpoint as the Pentair VSF speed command (BE16 RPM, subcmd
// 0x0A). Unlike flow_to_cmd there is no "0 = off" encoding: run/stop is a
// separate command, so a request below the drive's minimum is raised to it
// rather than silently becoming a stop.
inline int rpm_to_cmd(float rpm) {
  int cmd = (int) lroundf(rpm);
  if (cmd < RPM_CMD_MIN)
    cmd = RPM_CMD_MIN;
  if (cmd > RPM_CMD_MAX)
    cmd = RPM_CMD_MAX;
  return cmd;
}

}  // namespace pool_control
}  // namespace esphome
