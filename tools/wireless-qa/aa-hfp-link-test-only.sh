#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# TEST-ONLY operator step (#160). Brings the phone's HFP link up (or down) for a wireless test
# window, standing in for the infotainment system, which owns HFP in production. The Android
# Auto helper and receiver never do this themselves. Profile-scoped only: ConnectProfile /
# DisconnectProfile of the phone's HFP Audio Gateway, never generic Connect/Disconnect,
# pairing changes or RemoveDevice. Prints no Bluetooth addresses.
#
# usage: aa-hfp-link-test-only.sh connect|disconnect|status "<phone Bluetooth alias>"
#
# HFP is confirmed from WirePlumber's bluez5 device profile (headset-head-unit*), not from
# Device1.Connected: that is true for any bearer, and BlueZ 5.72 may answer ConnectProfile
# successfully after falling back to an LE ATT link on a dual-mode phone (src/device.c).
# Exit: 0 verified, 1 the action or its verification failed, 2 usage, 3 device not found.
set -euo pipefail
readonly HFP_AG=0000111f-0000-1000-8000-00805f9b34fb VERIFY_S=${AA_HFP_VERIFY_S:-15}
action=${1:-}; alias_name=${2:-}
case "$action" in connect|disconnect|status) ;; *) sed -n '9p' "$0" >&2; exit 2 ;; esac
[ -n "$alias_name" ] || { echo "aa-hfp-link-test-only: phone alias required" >&2; exit 2; }
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
quoted=$(printf '%q' "$alias_name")
fail() { echo "aa-hfp-link-test-only: $*" >&2; exit 1; }

matches=()
while read -r path; do
  value=$(busctl get-property org.bluez "$path" org.bluez.Device1 Alias 2>/dev/null || true)
  # busctl prints s "Alias" with shell-style escaping of quotes.
  value=${value#s \"}; value=${value%\"}; value=${value//\\\'/\'}; value=${value//\\\"/\"}
  [ "$value" = "$alias_name" ] && matches+=("$path")
done < <(busctl --list tree org.bluez 2>/dev/null | grep -E '^/org/bluez/hci[0-9]+/dev_[0-9A-F_]+$')
[ "${#matches[@]}" -eq 1 ] || { echo "aa-hfp-link-test-only: expected exactly one device named $quoted, found ${#matches[@]}" >&2; exit 3; }
device=${matches[0]}
[ "$(busctl get-property org.bluez "$device" org.bluez.Device1 Paired)" = "b true" ] || \
  { echo "aa-hfp-link-test-only: $quoted is not paired" >&2; exit 3; }
address=${device##*/dev_}; address=${address//_/:}   # internal only, never printed

# 0: WirePlumber's device for this phone has an HFP head-unit profile active; 1: it does not;
# 2: the PipeWire graph could not be read (treated as a verification failure).
hfp_active() {
  local dump
  dump=$(pw-dump 2>/dev/null) || return 2
  AA_HFP_ADDRESS=$address python3 -I -c '
import json, os, sys
address = os.environ["AA_HFP_ADDRESS"].upper()
try:
    objects = json.loads(sys.stdin.read())
except ValueError:
    sys.exit(2)
for obj in objects:
    info = obj.get("info") or {}
    props = info.get("props") or {}
    if obj.get("type") != "PipeWire:Interface:Device" or props.get("device.api") != "bluez5":
        continue
    if str(props.get("api.bluez5.address", "")).upper() != address:
        continue
    for profile in (info.get("params") or {}).get("Profile") or []:
        if str(profile.get("name", "")).startswith("headset-head-unit"):
            sys.exit(0)
sys.exit(1)' <<<"$dump"
}
# Polls until hfp_active returns the wanted status (0 up, 1 down) or the budget runs out.
await_hfp() {
  local want=$1 status
  for _ in $(seq 1 "$VERIFY_S"); do
    status=0; hfp_active || status=$?
    [ "$status" -eq 2 ] && fail "cannot read the PipeWire graph (pw-dump) to verify HFP"
    [ "$status" -eq "$want" ] && return 0
    sleep 1
  done
  return 1
}
link() { busctl get-property org.bluez "$device" org.bluez.Device1 Connected; }

case "$action" in
  status)
    status=0; hfp_active || status=$?
    case $status in 0) echo "HFP=up Connected=$(link)" ;; 1) echo "HFP=down Connected=$(link)" ;;
                    *) fail "cannot read the PipeWire graph (pw-dump)" ;; esac ;;
  connect)
    if ! err=$(sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 ConnectProfile s "$HFP_AG" 2>&1 >/dev/null); then
      # Only the D-Bus error name: BlueZ messages are not part of the redacted output.
      fail "ConnectProfile(HFP AG) failed: $(grep -oE 'org\.bluez\.Error\.[A-Za-z]+' <<<"$err" | head -1 || echo unknown)"
    fi
    await_hfp 0 || fail "ConnectProfile returned but no HFP profile became active within ${VERIFY_S}s (Connected=$(link)); an LE-only link is not HFP"
    echo "HFP=up Connected=$(link)" ;;
  disconnect)
    if ! err=$(sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 DisconnectProfile s "$HFP_AG" 2>&1 >/dev/null); then
      name=$(grep -oE 'org\.bluez\.Error\.[A-Za-z]+' <<<"$err" | head -1 || true)
      # Already down is fine; anything else is a failed teardown.
      [ "$name" = org.bluez.Error.NotConnected ] || fail "DisconnectProfile(HFP AG) failed: ${name:-unknown}"
    fi
    await_hfp 1 || fail "HFP still active ${VERIFY_S}s after DisconnectProfile"
    echo "HFP=down Connected=$(link)" ;;
esac
