#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# TEST-ONLY operator step (#160, #179). Brings the phone's HFP link up (or down) for a wireless
# test window, standing in for the infotainment system, which owns HFP in production. The Android
# Auto helper and receiver never do this themselves. Profile-scoped only: ConnectProfile /
# DisconnectProfile of the phone's HFP Audio Gateway, never generic Connect/Disconnect,
# pairing changes or RemoveDevice. Prints no Bluetooth addresses.
#
# usage: aa-hfp-link-test-only.sh connect|disconnect|status "<phone Bluetooth alias>"
#
# HFP is verified from bluetoothd's per-device service state transitions in the bluetooth unit
# journal (`journalctl -u bluetooth -o cat`, emitted by `bluetoothd -d`:
# `src/service.c:change_state() ... device <MAC> profile Hands-Free... state changed: ... -> ...`),
# anchored by a pre-action journal cursor (`--show-cursor` / `--after-cursor`) so same-second
# transitions are never missed and stale prior transitions never satisfy disconnect.
# `Device1.Connected=false` is also accepted as down evidence. `Device1.Connected=true` alone is
# NEVER proof of HFP: BlueZ 5.72 may answer ConnectProfile successfully after falling back to an
# LE ATT link on a dual-mode phone (src/device.c). WirePlumber/PipeWire (`pw-dump`) is secondary
# info only.
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

# Capture the current bluetooth journal cursor before mutating profile state so
# subsequent polls (`--after-cursor`) observe only transitions caused by this
# action, including transitions within the same second.
journal_cursor() {
  local out line
  out=$(journalctl -u bluetooth -n 0 --show-cursor -o cat 2>/dev/null) || return 1
  line=$(printf '%s\n' "$out" | grep -E '^-- cursor: ' | tail -n 1) || return 1
  line=${line#-- cursor: }
  [ -n "$line" ] || return 1
  printf '%s\n' "$line"
}

# Primary signal: bluetoothd -d per-device Hands-Free state transitions in the bluetooth journal.
# Only the local `Hands-Free unit` service (remote HFP AG, the UUID this tool connects) counts;
# `Hands-Free Voice gateway` is the opposite role and never proves this link (#198 review).
# Optional argument $1: `--after-cursor` value (empty reads the current boot's bluetooth journal).
# Prints "<code> <reason>" where code is:
#   0: latest Hands-Free transition for this device is -> connected
#   1: latest Hands-Free transition for this device is -> disconnected
#   2: journalctl could not be read (treated as unknown, never down)
#   3: unknown (no change_state lines, no Hands-Free transition for this device, or transient state)
journal_hfp_state() {
  local cursor=${1:-} logs
  if [ -n "$cursor" ]; then
    if ! logs=$(journalctl -u bluetooth -o cat --after-cursor "$cursor" 2>/dev/null); then
      echo "2 journal-unreadable"
      return 0
    fi
  else
    if ! logs=$(journalctl -u bluetooth -o cat -b 2>/dev/null); then
      echo "2 journal-unreadable"
      return 0
    fi
  fi
  AA_HFP_ADDRESS=$address python3 -I -c '
import os, re, sys

address = os.environ["AA_HFP_ADDRESS"].upper()
pattern = re.compile(
    r"change_state\(\).*?\bdevice\s+([0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5})\s+"
    r"profile\s+Hands-Free\s+unit\s+state changed:\s+\S+\s*->\s*([A-Za-z0-9_-]+)"
)
saw_change_state = False
latest = None
for line in sys.stdin:
    if "change_state()" in line:
        saw_change_state = True
    m = pattern.search(line)
    if m and m.group(1).upper() == address:
        latest = m.group(2).lower()

if latest == "connected":
    print("0 journal")
elif latest == "disconnected":
    print("1 journal")
elif latest is not None:
    print(f"3 journal-state-{latest}")
elif not saw_change_state:
    print("3 no-bluetoothd-change-state-lines")
else:
    print("3 no-hfp-transition-for-device")
' <<<"$logs"
}

# Secondary informational check: WirePlumber/PipeWire bluez5 graph. Never overrides journal/BlueZ.
pw_hfp_state() {
  local dump
  dump=$(pw-dump 2>/dev/null) || { echo "unreadable"; return 0; }
  AA_HFP_ADDRESS=$address python3 -I -c '
import json, os, sys
address = os.environ["AA_HFP_ADDRESS"].upper()
try:
    objects = json.loads(sys.stdin.read())
except ValueError:
    print("unreadable")
    sys.exit(0)
for obj in objects:
    info = obj.get("info") or {}
    props = info.get("props") or {}
    if obj.get("type") != "PipeWire:Interface:Device" or props.get("device.api") != "bluez5":
        continue
    if str(props.get("api.bluez5.address", "")).upper() != address:
        continue
    for profile in (info.get("params") or {}).get("Profile") or []:
        if str(profile.get("name", "")).startswith("headset-head-unit"):
            print("up")
            sys.exit(0)
    print("down")
    sys.exit(0)
print("absent")
' <<<"$dump"
}

link() { busctl get-property org.bluez "$device" org.bluez.Device1 Connected 2>/dev/null || echo "unknown"; }

case "$action" in
  status)
    conn=$(link)
    pw=$(pw_hfp_state)
    read -r j_code j_reason < <(journal_hfp_state "")
    if [ "$conn" = "b false" ]; then
      echo "HFP=down Connected=$conn (source=bluez-disconnected pw=$pw)"
    elif [ "$j_code" -eq 0 ] && [ "$conn" = "b true" ]; then
      echo "HFP=up Connected=$conn (source=$j_reason pw=$pw)"
    elif [ "$j_code" -eq 1 ]; then
      echo "HFP=down Connected=$conn (source=$j_reason pw=$pw)"
    else
      echo "HFP=unknown ($j_reason; pw=$pw) Connected=$conn"
    fi ;;
  connect)
    if ! cursor=$(journal_cursor); then
      echo "HFP=unknown (journal-cursor-unavailable; pw=$(pw_hfp_state)) Connected=$(link)"
      fail "cannot obtain bluetooth journal cursor to verify HFP"
    fi
    if ! err=$(sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 ConnectProfile s "$HFP_AG" 2>&1 >/dev/null); then
      # Only the D-Bus error name: BlueZ messages are not part of the redacted output.
      fail "ConnectProfile(HFP AG) failed: $(grep -oE 'org\.bluez\.Error\.[A-Za-z]+' <<<"$err" | head -1 || echo unknown)"
    fi
    j_code=3; j_reason="no-hfp-transition-for-device"
    for _ in $(seq 1 "$VERIFY_S"); do
      read -r j_code j_reason < <(journal_hfp_state "$cursor")
      if [ "$j_code" -eq 0 ]; then
        echo "HFP=up Connected=$(link) (source=$j_reason pw=$(pw_hfp_state))"
        exit 0
      fi
      sleep 1
    done
    fail "ConnectProfile returned but no HFP profile became active within ${VERIFY_S}s (Connected=$(link) journal=$j_reason pw=$(pw_hfp_state)); an LE-only link is not HFP" ;;
  disconnect)
    cursor=""
    cursor=$(journal_cursor) || cursor=""
    if ! err=$(sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 DisconnectProfile s "$HFP_AG" 2>&1 >/dev/null); then
      name=$(grep -oE 'org\.bluez\.Error\.[A-Za-z]+' <<<"$err" | head -1 || true)
      if [ "$name" = org.bluez.Error.NotConnected ]; then echo "HFP=down (BlueZ: not connected) Connected=$(link)"; exit 0; fi
      fail "DisconnectProfile(HFP AG) failed: ${name:-unknown}"
    fi
    j_code=3; j_reason="no-hfp-transition-for-device"
    for _ in $(seq 1 "$VERIFY_S"); do
      conn=$(link)
      if [ "$conn" = "b false" ]; then
        echo "HFP=down Connected=$conn (source=bluez-disconnected pw=$(pw_hfp_state))"
        exit 0
      fi
      if [ -n "$cursor" ]; then
        read -r j_code j_reason < <(journal_hfp_state "$cursor")
        if [ "$j_code" -eq 1 ]; then
          echo "HFP=down Connected=$conn (source=$j_reason pw=$(pw_hfp_state))"
          exit 0
        fi
      else
        j_code=2; j_reason="journal-cursor-unavailable"
      fi
      sleep 1
    done
    if [ "$j_code" -eq 0 ]; then
      fail "HFP still active ${VERIFY_S}s after DisconnectProfile"
    fi
    if [ -z "$cursor" ]; then
      echo "HFP=unknown (journal-cursor-unavailable; pw=$(pw_hfp_state)) Connected=$(link)"
    fi
    fail "unverifiable: no HFP disconnect transition after cursor ($j_reason) and Device1.Connected=$(link)" ;;
esac
