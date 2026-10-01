// Host unit tests for the pump / salt-cell safety decisions.
// Build & run:  c++ -std=c++17 -Wall esphome/tests/test_pump_interlock.cpp -o /tmp/pitest && /tmp/pitest
#include "../components/pool_control/pump_interlock.h"

#include <cassert>
#include <cstdio>

using esphome::pool_control::effective_pump_flow;
using esphome::pool_control::flow_to_cmd;
using esphome::pool_control::nonzero_timestamp_ms;
using esphome::pool_control::remaining_delay_ms;
using esphome::pool_control::rpm_to_cmd;
using esphome::pool_control::RPM_CMD_MAX;
using esphome::pool_control::RPM_CMD_MIN;
using esphome::pool_control::travel_stop_active;
using esphome::pool_control::TravelAction;
using esphome::pool_control::valve_travel_action;

int main() {
  const float creep = 29.0f;
  const float desired = 44.0f;

  {
    assert(nonzero_timestamp_ms(123) == 123);
    assert(nonzero_timestamp_ms(0) == 1);
  }

  {
    assert(remaining_delay_ms(/*now*/100, /*deadline*/200) == 100);
    assert(remaining_delay_ms(/*now*/200, /*deadline*/200) == 0);
    assert(remaining_delay_ms(/*now*/300, /*deadline*/200) == 0);
    assert(remaining_delay_ms(/*now*/0x80000001U, /*deadline*/0) == 0);
    assert(remaining_delay_ms(/*now*/0xFFFFFFF0U, /*deadline*/0x00000010U) == 32);
    assert(remaining_delay_ms(/*now*/0x00000010U, /*deadline*/0x00000010U) == 0);
  }

  // --- effective_pump_flow: valve-travel clamp ---
  {
    // Inside the window: clamp a high request down to the creep.
    assert(effective_pump_flow(desired, creep, /*now*/100, /*until*/200) == creep);
    // Exactly at the deadline: window has expired -> no clamp.
    assert(effective_pump_flow(desired, creep, /*now*/200, /*until*/200) == desired);
    // Past the deadline: no clamp.
    assert(effective_pump_flow(desired, creep, /*now*/300, /*until*/200) == desired);
    // No window set (until==0): never clamps.
    assert(effective_pump_flow(desired, creep, /*now*/0, /*until*/0) == desired);
    // Only ever reduces: a request already <= creep is untouched inside the window.
    assert(effective_pump_flow(18.0f, creep, /*now*/100, /*until*/200) == 18.0f);
    // Request equal to creep inside window -> unchanged (not > creep).
    assert(effective_pump_flow(creep, creep, /*now*/100, /*until*/200) == creep);
    assert(effective_pump_flow(desired, creep, /*now*/0xFFFFFFF0U,
                               /*until*/0x00000010U) == creep);
  }

  // --- flow_to_cmd: native GPM -> single-byte encoding (identity) ---
  {
    assert(flow_to_cmd(0.0f) == 0);
    assert(flow_to_cmd(44.0f) == 44);
    assert(flow_to_cmd(25.0f) == 25);
    // Rounds to nearest.
    assert(flow_to_cmd(44.4f) == 44);
    assert(flow_to_cmd(44.6f) == 45);
    // Clamps to the single-byte ceiling (255 GPM) and floor.
    assert(flow_to_cmd(255.0f) == 255);
    assert(flow_to_cmd(300.0f) == 255);
    assert(flow_to_cmd(-5.0f) == 0);
  }

  // --- rpm_to_cmd: native RPM -> VSF speed command ---
  {
    assert(rpm_to_cmd(1500.0f) == 1500);
    assert(rpm_to_cmd(1791.0f) == 1791);
    // Rounds to nearest.
    assert(rpm_to_cmd(1500.4f) == 1500);
    assert(rpm_to_cmd(1500.6f) == 1501);
    // Clamps to the drive's speed range at both ends.
    assert(rpm_to_cmd(RPM_CMD_MIN) == RPM_CMD_MIN);
    assert(rpm_to_cmd(RPM_CMD_MAX) == RPM_CMD_MAX);
    assert(rpm_to_cmd(4000.0f) == RPM_CMD_MAX);
    // No "0 == stop" encoding: stopping is a separate command, so a request
    // below the minimum is raised to the minimum, never turned into a stop.
    assert(rpm_to_cmd(200.0f) == RPM_CMD_MIN);
    assert(rpm_to_cmd(0.0f) == RPM_CMD_MIN);
    assert(rpm_to_cmd(-100.0f) == RPM_CMD_MIN);
  }

  // --- valve_travel_action: which protection a diverter move needs ---
  {
    const float travel = 30.0f;
    // Ordinary valve, pump running above the creep -> the usual dip.
    assert(valve_travel_action(false, true, false, desired, creep, travel) ==
           TravelAction::CREEP);
    // Ordinary valve already at/below the creep -> nothing to dip to.
    assert(valve_travel_action(false, true, false, creep, creep, travel) ==
           TravelAction::RUN);
    assert(valve_travel_action(false, true, false, 18.0f, creep, travel) ==
           TravelAction::RUN);
    // Speed mode is exempt from the dip: holding rpm is already below the creep.
    assert(valve_travel_action(false, true, true, desired, creep, travel) ==
           TravelAction::RUN);

    // Fountain valve -> full stop, in flow mode AND in speed mode, and
    // regardless of how low the current setpoint already is (a dribbling arc
    // is exactly what a reduced-but-nonzero flow produces).
    assert(valve_travel_action(true, true, false, desired, creep, travel) ==
           TravelAction::STOP);
    assert(valve_travel_action(true, true, true, desired, creep, travel) ==
           TravelAction::STOP);
    assert(valve_travel_action(true, true, false, 12.0f, creep, travel) ==
           TravelAction::STOP);

    // Guard disabled (travel 0 s) -> never touches the pump, fountain included.
    assert(valve_travel_action(true, true, false, desired, creep, 0.0f) ==
           TravelAction::RUN);
    assert(valve_travel_action(false, true, false, desired, creep, 0.0f) ==
           TravelAction::RUN);
    // Pump unpowered -> nothing to stop or dip.
    assert(valve_travel_action(true, false, false, desired, creep, travel) ==
           TravelAction::RUN);
    assert(valve_travel_action(false, false, false, desired, creep, travel) ==
           TravelAction::RUN);
  }

  // --- travel_stop_active: does an in-flight setpoint get held at zero? ---
  {
    // Inside a fountain window: hold stopped.
    assert(travel_stop_active(true, /*now*/100, /*until*/200));
    // Exactly at the deadline, and past it: window over, let the pump run.
    assert(!travel_stop_active(true, /*now*/200, /*until*/200));
    assert(!travel_stop_active(true, /*now*/300, /*until*/200));
    // An ordinary (creep) window never forces a stop -- effective_pump_flow
    // handles those by clamping instead.
    assert(!travel_stop_active(false, /*now*/100, /*until*/200));
    // No window open at all.
    assert(!travel_stop_active(true, /*now*/0, /*until*/0));
    assert(travel_stop_active(true, /*now*/0xFFFFFFF0U,
                              /*until*/0x00000010U));
  }

  std::printf("pump_interlock: all tests passed\n");
  return 0;
}
