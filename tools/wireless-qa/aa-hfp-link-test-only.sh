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
# HFP is verified primarily from BlueZ's D-Bus state (GetManagedObjects): per-profile / service
# state if available (Device1.ConnectedProfiles/ConnectedUUIDs, Service1/Profile1), otherwise
# MediaTransport1 for an HFP UUID, or Device1.Connected=false for down. Device1.Connected=true
# alone is NEVER proof of HFP: BlueZ 5.72 may answer ConnectProfile successfully after falling
# back to an LE ATT link on a dual-mode phone (src/device.c). WirePlumber/PipeWire (pw-dump) is
# kept as secondary information and fallback when BlueZ exposes no per-profile/transport state.
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

# Primary check: BlueZ D-Bus state via ObjectManager.GetManagedObjects.
# Prints "<status_code> <source>" where status_code is:
#   0: HFP verified up (bluez-profile, bluez-service, or bluez-transport)
#   1: HFP verified down (bluez-profile, bluez-service, or bluez-disconnected)
#   2: BlueZ D-Bus state could not be read
#   3: Device1.Connected is true, but BlueZ exposes no HFP profile/service/transport state
bluez_hfp_state() {
  local objects
  if ! objects=$(busctl --json=short call org.bluez / org.freedesktop.DBus.ObjectManager GetManagedObjects 2>/dev/null); then
    echo "2 bluez-unreadable"
    return 0
  fi
  AA_HFP_DEVICE=$device python3 -I -c '
import json, os, sys

HFP_UUIDS = {
    "0000111f-0000-1000-8000-00805f9b34fb",  # HFP Audio Gateway
    "0000111e-0000-1000-8000-00805f9b34fb",  # HFP Hands-Free
}
device = os.environ["AA_HFP_DEVICE"]

def unwrap(val):
    if isinstance(val, dict) and "data" in val and "type" in val:
        return val["data"]
    return val

try:
    payload = json.loads(sys.stdin.read())
except ValueError:
    print("2 bluez-unreadable")
    sys.exit(0)

data = payload.get("data") if isinstance(payload, dict) else None
objects = data[0] if isinstance(data, list) and data and isinstance(data[0], dict) else None
if objects is None:
    print("2 bluez-unreadable")
    sys.exit(0)

dev_ifaces = objects.get(device)
if not isinstance(dev_ifaces, dict):
    print("2 bluez-device-missing")
    sys.exit(0)

dev = dev_ifaces.get("org.bluez.Device1") or {}
connected = unwrap(dev.get("Connected"))

# 1a. Per-profile UUID list on Device1 if exposed by BlueZ.
for prop in ("ConnectedProfiles", "ConnectedUUIDs"):
    if prop in dev:
        raw = unwrap(dev.get(prop))
        if isinstance(raw, list):
            uuids = {str(u).lower() for u in raw}
            if uuids & HFP_UUIDS:
                print("0 bluez-profile")
            else:
                print("1 bluez-profile")
            sys.exit(0)

# 1b. Per-service / profile objects under or referencing the device.
service_down = False
for path, ifaces in objects.items():
    if not isinstance(ifaces, dict):
        continue
    for iface_name in ("org.bluez.Service1", "org.bluez.Profile1"):
        svc = ifaces.get(iface_name)
        if not isinstance(svc, dict):
            continue
        svc_dev = unwrap(svc.get("Device"))
        if svc_dev != device and path != device and not path.startswith(device + "/"):
            continue
        uuid = str(unwrap(svc.get("UUID")) or "").lower()
        if uuid not in HFP_UUIDS:
            continue
        svc_conn = unwrap(svc.get("Connected"))
        if isinstance(svc_conn, bool):
            if svc_conn:
                print("0 bluez-service")
                sys.exit(0)
            service_down = True
        svc_state = str(unwrap(svc.get("State")) or "").lower()
        if svc_state == "connected":
            print("0 bluez-service")
            sys.exit(0)
        if svc_state == "disconnected":
            service_down = True
if service_down:
    print("1 bluez-service")
    sys.exit(0)

# 2. MediaTransport1 for an HFP UUID on this device.
for path, ifaces in objects.items():
    if not isinstance(ifaces, dict):
        continue
    mt = ifaces.get("org.bluez.MediaTransport1")
    if not isinstance(mt, dict):
        continue
    mt_dev = unwrap(mt.get("Device"))
    if mt_dev != device and not (not mt_dev and path.startswith(device + "/")):
        continue
    uuid = str(unwrap(mt.get("UUID")) or "").lower()
    if uuid not in HFP_UUIDS:
        continue
    state = str(unwrap(mt.get("State")) or "").lower()
    if state in ("", "idle", "pending", "active"):
        print("0 bluez-transport")
        sys.exit(0)

# 3. Device1.Connected=false proves HFP is down; Connected=true alone is NOT HFP.
if connected is False:
    print("1 bluez-disconnected")
else:
    print("3 bluez-no-profile-state")
' <<<"$objects"
}

# Secondary check: WirePlumber/PipeWire bluez5 graph (informational; fallback only when
# BlueZ has Connected=true without per-profile/service/transport state).
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

# Combines BlueZ (primary) and PipeWire (secondary). Sets globals:
#   HFP_CODE (0=up, 1=down, 2=error, 3=unknown), HFP_SOURCE, HFP_PW.
probe_hfp() {
  local bz_code bz_src pw
  read -r bz_code bz_src < <(bluez_hfp_state)
  pw=$(pw_hfp_state)
  HFP_PW=$pw
  if [ "$bz_code" -eq 0 ] || [ "$bz_code" -eq 1 ] || [ "$bz_code" -eq 2 ]; then
    HFP_CODE=$bz_code
    HFP_SOURCE=$bz_src
    return 0
  fi
  # BlueZ returned 3 (Connected=true, no per-profile/service/transport state):
  # use PipeWire only as secondary confirmation if it sees the phone.
  case "$pw" in
    up)   HFP_CODE=0; HFP_SOURCE="pipewire" ;;
    down) HFP_CODE=1; HFP_SOURCE="pipewire" ;;
    *)    HFP_CODE=3; HFP_SOURCE=$bz_src ;;
  esac
}

# Polls until probe_hfp yields one of the wanted status codes or the budget runs out.
await_hfp() {
  for _ in $(seq 1 "$VERIFY_S"); do
    probe_hfp
    [ "$HFP_CODE" -eq 2 ] && fail "cannot read BlueZ D-Bus state (GetManagedObjects) to verify HFP"
    for want in "$@"; do [ "$HFP_CODE" -eq "$want" ] && return 0; done
    sleep 1
  done
  return 1
}
link() { busctl get-property org.bluez "$device" org.bluez.Device1 Connected; }

case "$action" in
  status)
    probe_hfp
    case "$HFP_CODE" in
      0) echo "HFP=up Connected=$(link) (source=$HFP_SOURCE pw=$HFP_PW)" ;;
      1) echo "HFP=down Connected=$(link) (source=$HFP_SOURCE pw=$HFP_PW)" ;;
      3) echo "HFP=unknown (no HFP profile/transport state in BlueZ; pw=$HFP_PW) Connected=$(link)" ;;
      *) fail "cannot read BlueZ D-Bus state (GetManagedObjects)" ;;
    esac ;;
  connect)
    if ! err=$(sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 ConnectProfile s "$HFP_AG" 2>&1 >/dev/null); then
      # Only the D-Bus error name: BlueZ messages are not part of the redacted output.
      fail "ConnectProfile(HFP AG) failed: $(grep -oE 'org\.bluez\.Error\.[A-Za-z]+' <<<"$err" | head -1 || echo unknown)"
    fi
    await_hfp 0 || fail "ConnectProfile returned but no HFP profile became active within ${VERIFY_S}s (Connected=$(link) pw=${HFP_PW:-unknown}); an LE-only link is not HFP"
    echo "HFP=up Connected=$(link) (source=$HFP_SOURCE pw=$HFP_PW)" ;;
  disconnect)
    # Absence only proves teardown against a baseline: HFP seen up first. Without one
    # (no profile/service/transport state and no bluez5 device), only BlueZ's own
    # NotConnected answer or Device1.Connected=false / profile-down proof counts.
    probe_hfp
    before=$HFP_CODE
    [ "$before" -eq 2 ] && fail "cannot read BlueZ D-Bus state (GetManagedObjects) to verify HFP"
    if ! err=$(sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 DisconnectProfile s "$HFP_AG" 2>&1 >/dev/null); then
      name=$(grep -oE 'org\.bluez\.Error\.[A-Za-z]+' <<<"$err" | head -1 || true)
      if [ "$name" = org.bluez.Error.NotConnected ]; then echo "HFP=down (BlueZ: not connected) Connected=$(link)"; exit 0; fi
      fail "DisconnectProfile(HFP AG) failed: ${name:-unknown}"
    fi
    if [ "$before" -eq 0 ]; then
      await_hfp 1 3 || fail "HFP still active ${VERIFY_S}s after DisconnectProfile"
    else
      # Not seen up first: explicit down proof (BlueZ profile/service/disconnected or PipeWire off) is required.
      await_hfp 1 || fail "unverifiable: HFP was not visible in BlueZ D-Bus or PipeWire before DisconnectProfile"
    fi
    echo "HFP=down Connected=$(link) (source=$HFP_SOURCE pw=$HFP_PW)" ;;
esac
