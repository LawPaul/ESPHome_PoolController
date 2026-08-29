# Pool Controller — Bill of Materials

This is the reference build used by this repository. Exact parts are retained
where the firmware, electrical design, connector compatibility, or
[panel layout](panel-layout.svg) depends on them. Commodity parts may be
substituted when their listed electrical and mechanical requirements are met.

## Core components

| Function | Reference part | Qty | Requirement / reason |
|---|---|---:|---|
| Controller | Waveshare ESP32-S3-ETH-8DI-8RO, SKU 31130 | 1 | ESP32-S3 controller with onboard isolated RS485, TCA9554-driven 8-channel Form-C relay bank, RTC, WiFi, and DIN mounting |
| 24 VDC supply | Mean Well HDR-30-24 | 1 | Powers the controller and four 24 VDC G7L coils; DIN rail |
| 24 VAC supply | Functional Devices TIB40A | 1 | 120-to-24 VAC, 40 VA, UL Class 2, DIN rail, built-in circuit breaker; powers four valve actuators |
| Power relays | Omron G7L-2A-BUBJ-CB, 24 VDC coil | 4 | Two-pole contacts; #1 pump + PC100, #2 valve-actuator 24 VAC master, #3 booster, #4 blower |
| Outdoor temperature probe | Adafruit 381 waterproof DS18B20, 6 mm tube | 1 | Known probe dimensions fit the M12 gland; used for freeze protection |
| Water temperature probe | Waterproof genuine DS18B20, 3-wire | 1 | In-pipe probe between pump and filter; select a probe and mounting method rated for the installation |
| RTC backup battery | Jauch ML1220 rechargeable 3 V Li/MnO2 | 1 | Correct rechargeable chemistry for the controller's RTC holder; do not substitute a primary CR1220 |

## Panel hardware

The Phoenix terminal parts and rail length below match the dimensions in
[panel-layout.svg](panel-layout.svg). Electrically equivalent substitutions are
possible, but their dimensions and terminal topology must be checked against
the panel layout first.

| Item | Reference part / specification | Qty | Notes |
|---|---|---:|---|
| Distribution blocks | Phoenix Contact UT 2,5-3PV, 3214262 | 6 | Three-level, six internally bonded clamp points; one each for L120, N120, DC+, DC-, ACH, and ACC |
| End clamps | Phoenix Contact CLIPFIX 35, 3022218 | 2 | Secures the terminal-block group to the rail |
| End cover | Phoenix Contact 3214314 | 1 | Closes the final UT 2,5-3PV block |
| DIN rail | NS 35/7.5, approximately 350 mm cut | 1 | Reference lineup is approximately 355 mm within a 362 mm usable width; verify before cutting |
| Exterior WiFi antenna | Outdoor-rated 2.4 GHz IP67 panel/bulkhead antenna, SMA | 1 | Must be SMA, not RP-SMA; mounted through the enclosure wall |
| Internal antenna jumper | Short SMA RG316 jumper | 1 | Connects the controller's SMA jack to the bulkhead antenna inside the enclosure |
| Actuator connectors | Mating 3-pin JST-XH pigtails with pre-crimped flying leads | 4 | Confirm connector gender and Red/White/Black pin order before wiring |
| Cable glands | M25 five-hole gland and M12 gland sized for the 6 mm Adafruit probe | 1 each | M25 carries RS485 plus four actuator cables; M12 clamps the outdoor probe tube |
| Water-probe mounting hardware | Pipe saddle/clamp, gasket, and seal compatible with the pipe and probe | 1 set | Must remain watertight at system pressure |

## Wiring and consumables

| Item | Qty | Requirement |
|---|---:|---|
| RS485 cable | As required | 22 AWG twisted pair suitable for the equipment environment; Belden 3106A is a tested example |
| Control wire | As required | Stranded 18 AWG with insulation rated for the enclosure and circuit |
| DS18B20 pull-up resistor | 1 | 4.7 kΩ, 1/4 W or greater |
| 120 V primary conductors | As required | 14 AWG for the short branch-fed PSU and transformer taps; follow applicable electrical code |
| Ferrules, labels, ties, and strain relief | As required | Sized for the selected conductors and terminals |
| Shield bonding conductor | As required | Bond cable shields at one end only as described in [wiring.md](wiring.md) |

## Controlled equipment

These devices describe the installation implemented by the supplied firmware
and dashboard. A different equipment set requires corresponding configuration,
wiring, and safety review.

| Equipment | Reference installation | Qty | Interface |
|---|---|---:|---|
| Filter pump | Pentair IntelliFlo3 VSF | 1 | 240 V through G7L #1; RS485 |
| Chlorinator power center and cell | Pentair PC100 + IntelliChlor Plus60 | 1 | 240 V from the load side of G7L #1; RS485 at PC100 |
| Cleaner booster pump | Polaris booster pump | 1 | G7L #3; verify nameplate voltage and current |
| Spa blower | Polaris blower | 1 | G7L #4; verify nameplate voltage and current |
| Valve actuators | Hayward 3-wire 24 VAC reversing actuator | 2 | Direction through onboard relays 5-8; master power through G7L #2 |
| Valve actuators | Tork 3-wire 24 VAC reversing actuator | 2 | Direction through onboard relays 5-8; master power through G7L #2 |

## Optional inputs and spares

| Item | Qty | Notes |
|---|---:|---|
| Flow or pressure switch | As needed | Connect to an isolated digital input and update the firmware interlock accordingly |
| Spare RTC battery | 1 | Use rechargeable ML1220 chemistry only |

## Network choice

The reference build uses WiFi and leaves the RJ45 port unused. The controller's
external-antenna module has no PCB antenna, so the SMA antenna and internal
jumper are required for this build. Mount the antenna through the enclosure,
seal the penetration, weatherproof any exposed connector, and provide a drip
loop.

See [wiring.md](wiring.md) for terminal-level connections, electrical safety
notes, relay allocation, and commissioning checks.
