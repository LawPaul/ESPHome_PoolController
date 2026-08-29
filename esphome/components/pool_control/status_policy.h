#pragma once

namespace esphome {
namespace pool_control {

// Pure status-LED precedence. Picks a single glanceable state by severity; the
// YAML lambda maps the returned state to an RGB colour + brightness and drives
// the light. Keeping the precedence here (not in a lambda) makes the ordering
// unit-tested — the presentation (colours) stays in YAML.

enum StatusState {
  STATUS_HEALTHY = 0,  // green  - healthy / idle
  STATUS_SERVICE,      // blue   - manual maintenance override
  STATUS_COMMS,        // white  - RS485 comms lost
  STATUS_ALARM,        // red    - active alarm
  STATUS_FREEZE,       // cyan   - freeze protection
  STATUS_CLEANING,     // purple - cleaning cycle
  STATUS_BOOST,        // orange - chlorine boost
  STATUS_WARN,         // yellow - warning (salt / cold cell / clean cell / filter)
};

// Severity precedence, highest first. Service outranks everything (a technician
// has taken control); comms loss outranks alarms because a stale link makes the
// alarm state itself untrustworthy.
inline int status_state(bool service, bool comms, bool alarm, bool freeze,
                        bool cleaning, bool boost, bool warn) {
  if (service)
    return STATUS_SERVICE;
  if (comms)
    return STATUS_COMMS;
  if (alarm)
    return STATUS_ALARM;
  if (freeze)
    return STATUS_FREEZE;
  if (cleaning)
    return STATUS_CLEANING;
  if (boost)
    return STATUS_BOOST;
  if (warn)
    return STATUS_WARN;
  return STATUS_HEALTHY;
}

}  // namespace pool_control
}  // namespace esphome
