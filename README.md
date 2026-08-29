# ESPHome Pool Controller

This repository contains the ESPHome configuration, custom control logic,
wiring documents, and Home Assistant dashboard used for one specific pool and
spa installation. The controller hardware is a Waveshare
ESP32-S3-ETH-8DI-8RO. The installed Pentair pump and chlorinator communicate
over RS-485; relays control valve actuators and auxiliary loads.

The reusable Pentair RS-485 integration lives separately in
[ESPHome_PentairPool](https://github.com/LawPaul/ESPHome_PentairPool). This
repository contains the installation-specific control logic, hardware design,
dashboard, and tests.

## Scope and reuse

This is not a drop-in configuration for an arbitrary pool. It is specific to my equipment, plumbing topology, relay assignments, sensor addresses,
volume, measured hydraulic behavior, operating schedules, and
Home Assistant entity IDs. Some control decisions also depend on the safety
devices and electrical interlocks present in this installation.

The repository can be used as a template or reference for another controller,
but those assumptions must be reviewed and adapted first. In particular, do
not reuse the wiring, GPIO assignments, valve sequencing, flow calibration,
chlorination parameters, or freeze-protection settings without validating them
against the target pool and applicable equipment manuals.

## Implemented system

- IntelliFlo3 pump and IntelliChlor control over RS-485
- Pool, spa, cleaning, freeze-protection, and service modes
- Interlocked valve, pump, chlorinator, booster, and blower control
- Low-flow operation using RPM control with flow feedback
- Chlorine-demand tracking and configurable generation targets
- Home Assistant dashboard and service controls
- Host-side C++ tests for the framework-independent control logic

## Repository layout

- [esphome/pool-controller.yaml](esphome/pool-controller.yaml) — main ESPHome configuration
- [esphome/components/pool_control](esphome/components/pool_control) — custom control logic
- [esphome/tests](esphome/tests) — host-side unit tests
- [ha/pool-dashboard.yaml](ha/pool-dashboard.yaml) — Home Assistant dashboard
- [wiring.md](wiring.md) — wiring and service documentation
- [bill-of-materials.md](bill-of-materials.md) — reference hardware
- [panel-layout.svg](panel-layout.svg) — enclosure layout

## Setup

Use Python 3.12 with ESPHome. Create an untracked `esphome/secrets.yaml`:

```yaml
wifi_ssid: "your-network"
wifi_password: "your-password"
ap_password: "fallback-access-point-password"
api_key: "your-32-byte-base64-api-key"
ota_password: "your-ota-password"
```

Before building, adapt the configuration to the target hardware and pool as
described above. At minimum, review all substitutions, GPIO assignments,
equipment addresses, sensor addresses, plumbing states, relay interlocks,
schedules, hydraulic and chlorination parameters, and Home Assistant entity
IDs. Then validate and compile:

```sh
esphome config esphome/pool-controller.yaml
esphome compile esphome/pool-controller.yaml
```

The dashboard uses Mushroom, ApexCharts Card, card-templater, Blitzortung Card,
and vertical-stack-in-card custom resources.

## Safety

This project controls pumps, valve actuators, auxiliary loads, and chlorination
equipment connected to mains power and a pressurized water system. The wiring
documents describe only the installation represented by this repository; they
are not a universal design or installation instruction. Use correctly rated
components, preserve independent equipment safety mechanisms, and have the
installation reviewed and performed as required by local electrical codes.
