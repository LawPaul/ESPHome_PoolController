#!/usr/bin/env bash
#
# hotspot-logs.sh — recover diagnostics from the pool controller when it has
# fallen back to its own Wi-Fi AP and is unreachable on the LAN.
#
# Joins "Pool Controller Hotspot", captures (a) a full entity-state snapshot
# from the on-device web server and (b) a live log stream from the ESPHome API,
# then puts this Mac back on the house network. The restore runs from an EXIT
# trap, so it happens even if a capture step fails or you Ctrl-C.
#
# NOTE: ESPHome keeps NO log history. The log capture starts when we connect,
# so it shows the device's CURRENT behaviour, not the original disconnect. The
# state snapshot is the part that carries evidence from before — above all the
# Uptime sensor, which says whether the device rebooted or merely lost the link.
#
# Usage:  scripts/hotspot-logs.sh [log_seconds]     (default 45)
#
set -uo pipefail

IFACE="${IFACE:-en0}"
AP_SSID="Pool Controller Hotspot"
DEV_IP="192.168.4.1"
LOG_SECS="${1:-45}"
SNAP_SECS=12

PROJ="$HOME/pool-controller"
CFG_DIR="$PROJ/esphome"
CFG="pool-controller.yaml"
ESPHOME="$PROJ/.venv/bin/esphome"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUTDIR="$PROJ/diagnostics"
SNAP="$OUTDIR/state-$STAMP.txt"
LOG="$OUTDIR/log-$STAMP.txt"

mkdir -p "$OUTDIR"

# Pull credentials out of secrets.yaml. Values are never echoed.
secret() {
  awk -v k="$1:" '$1==k {sub(/^[^:]*:[[:space:]]*/,""); gsub(/^"|"$/,""); print; exit}' \
    "$CFG_DIR/secrets.yaml"
}
AP_PW="$(secret ap_password)"
HOME_SSID="$(secret wifi_ssid)"
HOME_PW="$(secret wifi_password)"

if [[ -z "$AP_PW" || -z "$HOME_SSID" || -z "$HOME_PW" ]]; then
  echo "ERROR: could not read ssid/passwords from $CFG_DIR/secrets.yaml" >&2
  exit 1
fi

# Recent macOS refuses to report the current SSID without Location permission
# (`networksetup -getairportnetwork` answers "not associated" even when it is),
# so every check here is by IP instead: the house LAN is whatever prefix we were
# on at start, and the fallback AP hands out 192.168.4.x.
iface_ip() { ipconfig getifaddr "$IFACE" 2>/dev/null; }

on_prefix() { [[ "$(iface_ip)" == "$1"* ]]; }

# Captured before the trap is armed so the restore path can never run against
# unset values under `set -u`.
ORIG_IP="$(iface_ip)"
ORIG_GW="$(route -n get default 2>/dev/null | awk '/gateway/{print $2}')"
HOME_PREFIX="${ORIG_IP%.*}."
if [[ -z "$ORIG_IP" || -z "$ORIG_GW" ]]; then
  echo "ERROR: $IFACE is not on a network right now; connect to $HOME_SSID first" >&2
  exit 1
fi

restore_wifi() {
  echo
  echo "==> Restoring $HOME_SSID on $IFACE"
  networksetup -setairportnetwork "$IFACE" "$HOME_SSID" "$HOME_PW" >/dev/null 2>&1
  for _ in $(seq 1 30); do
    if on_prefix "$HOME_PREFIX" && ping -c1 -t2 "$ORIG_GW" >/dev/null 2>&1; then
      echo "==> Back on the house network ($(iface_ip), gw $ORIG_GW)"
      return 0
    fi
    sleep 1
  done
  local now_ip
  now_ip="$(iface_ip)"
  echo "!!! Could not confirm reconnection (current IP: ${now_ip:-none})" >&2
  echo "!!! Reconnect from the Wi-Fi menu if needed." >&2
  return 1
}
trap restore_wifi EXIT INT TERM

echo "==> Currently: $ORIG_IP via $ORIG_GW (prefix $HOME_PREFIX)"
echo "==> Joining \"$AP_SSID\""
if ! networksetup -setairportnetwork "$IFACE" "$AP_SSID" "$AP_PW"; then
  echo "ERROR: failed to join $AP_SSID (is the hotspot in range?)" >&2
  exit 1
fi

echo "==> Waiting for $DEV_IP"
online=0
for _ in $(seq 1 30); do
  if ping -c1 -t1 "$DEV_IP" >/dev/null 2>&1; then online=1; break; fi
  sleep 1
done
if [[ "$online" -ne 1 ]]; then
  echo "ERROR: $DEV_IP did not respond after joining the hotspot" >&2
  exit 1
fi
echo "==> Device reachable (this Mac is $(iface_ip))"

# (a) Entity-state snapshot via the on-device web server. /events is an SSE
# stream that emits every entity's current state on connect, so a few seconds
# is enough for a complete picture.
echo "==> Capturing entity state (${SNAP_SECS}s) -> $SNAP"
curl -sN --max-time "$SNAP_SECS" "http://$DEV_IP/events" > "$SNAP" 2>/dev/null || true

# (b) Live log stream via the ESPHome API (encryption key comes from the config).
echo "==> Capturing logs (${LOG_SECS}s) -> $LOG"
( cd "$CFG_DIR" && "$ESPHOME" logs "$CFG" --device "$DEV_IP" ) > "$LOG" 2>&1 &
log_pid=$!
( sleep "$LOG_SECS"; pkill -P "$log_pid" 2>/dev/null; kill "$log_pid" 2>/dev/null ) &
timer_pid=$!
wait "$log_pid" 2>/dev/null
kill "$timer_pid" 2>/dev/null

# Summary. Uptime is the headline: it distinguishes "rebooted then failed to
# rejoin" from "kept running and silently lost the link".
echo
echo "===================== SUMMARY ====================="
up_s="$(sed -n 's/.*"id":"sensor-uptime".*"value":\([0-9.]*\).*/\1/p' "$SNAP" | tail -1)"
if [[ -n "$up_s" ]]; then
  printf 'Uptime: %s s  (~%.1f h)\n' "$up_s" "$(echo "$up_s/3600" | bc -l)"
else
  echo "Uptime: not found in snapshot (grep '$SNAP' for \"uptime\")"
fi
grep -oE '"id":"[a-z_]*-[a-z0-9_]*","value":"[^"]*"' "$SNAP" 2>/dev/null |
  grep -iE "comms|fault|alarm|needed|pending|wifi" | sort -u | head -20
echo
echo "Log lines mentioning wifi/disconnect/reboot:"
grep -iE "wifi|disconnect|reason|reboot|AP mode|fallback" "$LOG" 2>/dev/null | tail -25
echo "==================================================="
echo
echo "Full files:"
echo "  $SNAP"
echo "  $LOG"
