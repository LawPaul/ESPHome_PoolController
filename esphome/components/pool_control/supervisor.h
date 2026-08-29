#pragma once

#include "status_policy.h"

namespace esphome {
namespace pool_control {

// The always-on supervisor tick. These are the decisions that hold CONTINUOUSLY,
// regardless of which timed sequence (scene / freeze / cleaning) currently owns
// the plumbing and pump. Consolidating them into one pure function makes the
// cross-cutting interactions a single host-tested truth table.
//
// KEY SEMANTIC: the booster dry-run guard is deliberately INDEPENDENT of
// service mode. Service suspends AUTOMATION (freeze / chlorine dosing /
// cleaning schedule) so a technician has manual control, but it must never
// defeat an interlock that keeps equipment from running dry. Only the
// annunciation (status LED) reacts to service. (Salt-cell dry-run protection is
// now hardware: the cell is powered from the pump relay's load side, and the
// pentair hub commands 0% output whenever the pump isn't running.)

struct SupervisorInputs {
  bool pump_running;    // pump reporting motion (NOTE: true during priming too)
  bool pump_priming;    // pump spinning up -- flow not yet established
  bool pump_alarm;      // pump fault flag (over-temp/current/voltage/etc.)

  // Annunciation state (status-LED precedence inputs).
  bool service;
  bool freeze;
  bool cleaning;
  bool boost;
  bool comms_lost;      // either peer's link is stale
  bool alarm;           // any hard alarm
  bool warn;            // any soft warning
};

struct SupervisorOutputs {
  bool booster_permit;  // MECHANICAL interlock (pump must run); service-independent
  int status_led;       // annunciation state (see status_policy.h)
};

inline SupervisorOutputs supervise(const SupervisorInputs &in) {
  SupervisorOutputs o{};
  // Booster (Polaris) must never run dry: permitted only while the pump is
  // GENUINELY running -- not merely priming (running() stays true during the
  // prime spin-up, before flow is established) -- and free of a pump fault.
  o.booster_permit = in.pump_running && !in.pump_priming && !in.pump_alarm;
  // Glanceable status colour by severity precedence.
  o.status_led = status_state(in.service, in.comms_lost, in.alarm, in.freeze,
                              in.cleaning, in.boost, in.warn);
  return o;
}

}  // namespace pool_control
}  // namespace esphome
