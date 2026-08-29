#!/usr/bin/env python3
"""Characterise a plumbing combo: find the salt cell's flow-switch thresholds.

WHY. The IntelliFlo3 refuses commanded GPM below 20 in flow mode (proven on this
unit, see ESPHome_PentairPool/components/pentair/protocol.h), so running slower
means rpm mode -- which is open-loop. Before we can hold a flow target safely we
need to know how far down there is to go, i.e. where the IntelliChlor's own flow
switch opens.
The commonly quoted figure is ~15 GPM but that is not measured on this cell; at
20 GPM `no_flow` is confirmed OFF, which is all we actually know today.

WHAT IT PRODUCES, per step: commanded rpm, reported rpm, reported GPM, watts and
the cell's `no_flow` bit. Two numbers matter:

  OPEN point    - stepping DOWN, where no_flow asserts.
  RECLOSE point - stepping back UP, where it clears.

The gap between them is the switch's hysteresis, and the flow loop must keep its
target above the RECLOSE point (not the open point) or it will chatter the cell
on and off at the boundary.

It also records rpm/GPM pairs, which give K = rpm/GPM for this combo -- the
geometry term the flow loop learns (components/pool_control/flow_learn.h). So
run it once per combo you intend to run slowly: idle skimmer, then the cleaning
spillover phase.

SAFETY. Actuation requires Service Mode (the Pump RPM Setpoint number is gated
on service_active()), which this script verifies rather than sets -- arming the
pad is a human decision. The pump's GPM is a model-derived ESTIMATE, so the
cell's no_flow bit is treated as the only ground truth. On any exit -- normal,
error or Ctrl-C -- the starting rpm is restored.

USAGE
    .venv/bin/python tools/flow_sweep.py --label idle-skimmer
    .venv/bin/python tools/flow_sweep.py --label spillover --k 110

Turn Service Mode ON first, and put the diverters in the combo you want to
characterise. Turn Service Mode OFF afterwards to hand control back.
"""

import argparse
import csv
import json
import statistics
import sys
import time
import urllib.parse
import urllib.request

DEFAULT_HOST = "pool-controller.local"

# Entities we sample. Keys are the web_server ids.
SENSORS = ("sensor-pump_rpm", "sensor-pump_flow", "sensor-pump_power")
BINARIES = ("binary_sensor-no_flow", "binary_sensor-pump_running")


def http_get(host, path, timeout=8):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode())


def set_number(host, entity, value, timeout=8):
    """POST a number setpoint. ESPHome web_server v3 takes value as a query arg."""
    qs = urllib.parse.urlencode({"value": value})
    req = urllib.request.Request(
        f"http://{host}/number/{entity}/set?{qs}", method="POST", data=b""
    )
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.status


def sample(host, seconds, settle_frac=0.4):
    """Hold for `seconds`, collecting the SSE stream, and AVERAGE the tail.

    Averaging is the point. The pump's GPM is a model-derived estimate that
    quantises coarsely and gets noisier as speed drops -- exactly the regime
    this sweep explores -- so a single instantaneous sample near the threshold
    could be off by more than the margin we are trying to measure. Each dwell
    therefore yields a MEAN over many samples plus the spread, and the spread
    is itself a result: it sizes the deadband the flow loop will need.

    The first `settle_frac` of the window is discarded so the pump's ramp to
    the new setpoint is not averaged in with the steady state.

    no_flow is handled differently: it is a latch, not an average. Any assert
    anywhere in the window counts, including the settle portion -- a switch
    that flickers at the boundary has still told us we are too low.
    """
    series = {k: [] for k in SENSORS}
    flags = {k: [] for k in BINARIES}
    t0 = time.time()
    settle_until = t0 + seconds * settle_frac
    deadline = t0 + seconds
    try:
        with urllib.request.urlopen(
            f"http://{host}/events", timeout=seconds + 5
        ) as stream:
            for raw in stream:
                now = time.time()
                if now >= deadline:
                    break
                line = raw.decode(errors="replace").strip()
                if not line.startswith("data:"):
                    continue
                try:
                    obj = json.loads(line[5:].strip())
                except json.JSONDecodeError:
                    continue
                ident = obj.get("id")
                if ident in BINARIES:
                    flags[ident].append(as_bool(obj))
                elif ident in SENSORS and now >= settle_until:
                    val = as_float(obj)
                    if val == val:  # drop NaN
                        series[ident].append(val)
    except Exception as exc:  # noqa: BLE001 - a dropped stream must not actuate
        print(f"  ! event stream error: {exc}", file=sys.stderr)
    return series, flags


def as_float(entry):
    if not entry:
        return float("nan")
    try:
        return float(entry.get("value"))
    except (TypeError, ValueError):
        return float("nan")


def as_bool(entry):
    if not entry:
        return None
    val = entry.get("value")
    if isinstance(val, bool):
        return val
    return str(val).upper() in ("ON", "TRUE", "1")


def mean_sd(values):
    if not values:
        return float("nan"), float("nan"), 0
    if len(values) == 1:
        return values[0], 0.0, 1
    return statistics.fmean(values), statistics.pstdev(values), len(values)


def read_step(host, dwell):
    series, flags = sample(host, dwell)
    rpm, rpm_sd, rpm_n = mean_sd(series["sensor-pump_rpm"])
    gpm, gpm_sd, gpm_n = mean_sd(series["sensor-pump_flow"])
    watts, watts_sd, _ = mean_sd(series["sensor-pump_power"])
    nf = [f for f in flags["binary_sensor-no_flow"] if f is not None]
    run = [f for f in flags["binary_sensor-pump_running"] if f is not None]
    return {
        "rpm": rpm, "rpm_sd": rpm_sd,
        "gpm": gpm, "gpm_sd": gpm_sd, "gpm_n": gpm_n,
        "watts": watts, "watts_sd": watts_sd,
        # Latched: any assert in the window is a trip.
        "no_flow": any(nf) if nf else None,
        "no_flow_final": nf[-1] if nf else None,
        "running": run[-1] if run else None,
        "samples": rpm_n,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--host",
        default=DEFAULT_HOST,
        help=f"controller hostname or IP (default: {DEFAULT_HOST})",
    )
    ap.add_argument("--label", required=True, help="combo name, e.g. idle-skimmer")
    ap.add_argument("--k", type=float, default=72.85,
                    help="rpm per GPM for this combo; sets the step ladder. "
                         "Measured 2026-07-25 on the skimmer leg: 1457rpm/20GPM.")
    ap.add_argument("--start-gpm", type=float, default=19.0)
    ap.add_argument("--floor-gpm", type=float, default=11.0,
                    help="stop descending here even if no_flow never asserts")
    ap.add_argument("--dwell", type=int, default=90,
                    help="seconds per step, both directions. The first 40%% is "
                         "discarded as settle, so 90 leaves ~54s averaged "
                         "(~27 samples) after the pump has finished ramping.")
    ap.add_argument("--no-exit-service", dest="exit_service",
                    action="store_false",
                    help="leave Service Mode ON at the end (for back-to-back "
                         "runs on another combo). Default is to turn it off "
                         "and resume automation.")
    ap.add_argument("--out", default=None, help="CSV output path")
    args = ap.parse_args()

    host = args.host
    out_path = args.out or f"/tmp/flow_sweep_{args.label}.csv"

    def rpm_for(gpm):
        # 25 rpm is the setpoint step the number enforces.
        return int(round(args.k * gpm / 25.0) * 25)

    # --- preconditions -----------------------------------------------------
    svc = http_get(host, "/switch/service_mode")
    if not svc.get("value"):
        sys.exit("ABORT: Service Mode is OFF. The rpm setpoint is service-gated; "
                 "turn Service Mode ON (and set the diverters for this combo) first.")

    start = read_step(host, 10)
    if not start["running"]:
        sys.exit("ABORT: pump is not running. In Service Mode the pump is stopped; "
                 "set Pump RPM Setpoint once by hand to start it, then re-run.")
    start_rpm = int(start["rpm"]) if start["rpm"] == start["rpm"] else rpm_for(20)
    print(f"Service Mode ON, pump running at {start_rpm} rpm / "
          f"{start['gpm']:.0f} GPM / {start['watts']:.0f} W. no_flow={start['no_flow']}")
    if start["no_flow"]:
        sys.exit("ABORT: no_flow is ALREADY asserted before the sweep starts. "
                 "Nothing to measure -- investigate first.")

    rows = []
    open_point = None
    reclose_point = None

    def record(phase, cmd_rpm, obs):
        row = {"phase": phase, "cmd_rpm": cmd_rpm,
               "rpm": round(obs["rpm"], 1), "rpm_sd": round(obs["rpm_sd"], 2),
               "gpm": round(obs["gpm"], 2), "gpm_sd": round(obs["gpm_sd"], 2),
               "watts": round(obs["watts"], 1),
               "watts_sd": round(obs["watts_sd"], 2),
               "samples": obs["samples"], "no_flow": obs["no_flow"]}
        rows.append(row)
        print(f"  {phase:4s} cmd={cmd_rpm:5d}  rpm={obs['rpm']:7.0f}  "
              f"gpm={obs['gpm']:5.2f}+-{obs['gpm_sd']:.2f}  "
              f"W={obs['watts']:6.0f}  n={obs['samples']:3d}  "
              f"no_flow={'YES' if obs['no_flow'] else 'no'}")
        return row

    try:
        # --- descend: find where the switch OPENS --------------------------
        print(f"\nDescending from {args.start_gpm} GPM "
              f"(floor {args.floor_gpm}), {args.dwell}s per step:")
        gpm = args.start_gpm
        first = True
        while gpm >= args.floor_gpm - 1e-9:
            cmd = rpm_for(gpm)
            set_number(host, "pump_rpm_setpoint", cmd)
            obs = read_step(host, args.dwell)

            if first:
                # The number is optimistic:false and reads back pump_desired_rpm,
                # so a stuck 0 means the service gate refused to actuate.
                echo = http_get(host, "/number/pump_rpm_setpoint")
                if str(echo.get("value")) in ("0", "0.0", ""):
                    sys.exit("ABORT: setpoint did not take (reads 0). The service "
                             "gate rejected it -- is Service Mode still ON?")
                first = False

            record("down", cmd, obs)
            if obs["no_flow"]:
                open_point = (cmd, obs["gpm"])
                print(f"\n  >> OPEN point: no_flow asserted at cmd {cmd} rpm "
                      f"(reported {obs['gpm']:.1f} GPM)")
                break
            gpm -= 0.5

        if open_point is None:
            print(f"\n  >> no_flow never asserted down to {args.floor_gpm} GPM. "
                  f"The switch is below the swept range -- lower --floor-gpm to "
                  f"find it, or take this as good news for the flow target.")
        else:
            # --- ascend: find where it RECLOSES ----------------------------
            print("\nAscending to find the reclose point (hysteresis):")
            cmd = open_point[0]
            while cmd < rpm_for(args.start_gpm):
                cmd += 25
                set_number(host, "pump_rpm_setpoint", cmd)
                obs = record("up", cmd, read_step(host, args.dwell))
                if not obs["no_flow"]:
                    reclose_point = (cmd, obs["gpm"])
                    print(f"\n  >> RECLOSE point: no_flow cleared at cmd {cmd} rpm "
                          f"(reported {obs['gpm']:.1f} GPM)")
                    break

    except KeyboardInterrupt:
        print("\ninterrupted -- restoring", file=sys.stderr)
    finally:
        print(f"\nRestoring pump to {start_rpm} rpm.")
        try:
            set_number(host, "pump_rpm_setpoint", start_rpm)
        except Exception as exc:  # noqa: BLE001
            print(f"  ! RESTORE FAILED: {exc}\n  ! Set Pump RPM Setpoint by hand, "
                  f"or turn Service Mode OFF to re-resolve the scene.", file=sys.stderr)

        if rows:
            with open(out_path, "w", newline="") as fh:
                w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
                w.writeheader()
                w.writerows(rows)
            print(f"Wrote {len(rows)} samples to {out_path}")

        # K for this combo, from the clean samples above the switch.
        good = [r for r in rows if not r["no_flow"] and r["gpm"] > 0
                and r["rpm"] == r["rpm"]]
        if good:
            ks = [r["rpm"] / r["gpm"] for r in good]
            print(f"K (rpm per GPM) for '{args.label}': "
                  f"min {min(ks):.1f}  max {max(ks):.1f}  "
                  f"mean {sum(ks)/len(ks):.1f}")
            noise = [r["gpm_sd"] for r in good if r["gpm_sd"] == r["gpm_sd"]]
            if noise:
                print(f"GPM noise (sd within a dwell): median "
                      f"{statistics.median(noise):.2f}, worst {max(noise):.2f} "
                      f"-- the flow loop's deadband must exceed this.")

        print("\nSUMMARY")
        print(f"  open    : {open_point}")
        print(f"  reclose : {reclose_point}")
        print("  Flow target must sit above the RECLOSE flow, with margin for "
              "the GPM estimate's error.")

        # Hand the pad back. Leaving Service Mode on is not a neutral state --
        # it suspends freeze protection and chlorination for as long as it is
        # forgotten -- so returning to automatic is the safe default. Turning
        # it off runs apply_base_mode, which re-resolves the scene and puts the
        # pump back into flow mode; the rpm we restored above is superseded.
        if args.exit_service:
            try:
                urllib.request.urlopen(urllib.request.Request(
                    f"http://{host}/switch/service_mode/turn_off",
                    method="POST", data=b""), timeout=8)
                print("  Service Mode OFF -- automation resumed "
                      "(apply_base_mode re-resolves the scene).")
            except Exception as exc:  # noqa: BLE001
                print(f"  ! could not turn Service Mode off: {exc}\n"
                      f"  ! DO IT BY HAND -- freeze protection and "
                      f"chlorination stay suspended until you do.",
                      file=sys.stderr)
        else:
            print("  Service Mode LEFT ON (--no-exit-service). Freeze "
                  "protection and chlorination stay suspended until you "
                  "turn it off.")


if __name__ == "__main__":
    main()
