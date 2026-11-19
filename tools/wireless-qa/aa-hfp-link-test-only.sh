#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# TEST-ONLY operator step (#160). Brings the phone's HFP link up (or down) for a wireless test
# window, standing in for the infotainment system, which owns HFP in production. The Android
# Auto helper and receiver never do this themselves. Profile-scoped only: ConnectProfile /
# DisconnectProfile of the phone's HFP Audio Gateway, never generic Connect/Disconnect,
# pairing changes or RemoveDevice. Prints no Bluetooth addresses.
#
# usage: aa-hfp-link-test-only.sh connect|disconnect|status "<phone Bluetooth alias>"
set -euo pipefail
readonly HFP_AG=0000111f-0000-1000-8000-00805f9b34fb
action=${1:-}; alias_name=${2:-}
case "$action" in connect|disconnect|status) ;; *) sed -n '3,9p' "$0" >&2; exit 2 ;; esac
[ -n "$alias_name" ] || { echo "aa-hfp-link-test-only: phone alias required" >&2; exit 2; }
quoted=$(printf '%q' "$alias_name")
matches=()
while read -r path; do
  value=$(busctl get-property org.bluez "$path" org.bluez.Device1 Alias 2>/dev/null || true)
  # busctl prints s "Alias" with shell-style escaping of quotes.
  value=${value#s \"}; value=${value%\"}; value=${value//\\\'/\'}; value=${value//\\\"/\"}
  [ "$value" = "$alias_name" ] && matches+=("$path")
done < <(busctl --list tree org.bluez 2>/dev/null | grep -E '^/org/bluez/hci[0-9]+/dev_[0-9A-F_]+$')
[ "${#matches[@]}" -eq 1 ] || { echo "aa-hfp-link-test-only: expected exactly one device named $quoted, found ${#matches[@]}" >&2; exit 3; }
device=${matches[0]}
paired=$(busctl get-property org.bluez "$device" org.bluez.Device1 Paired)
[ "$paired" = "b true" ] || { echo "aa-hfp-link-test-only: $quoted is not paired" >&2; exit 3; }
connected() { busctl get-property org.bluez "$device" org.bluez.Device1 Connected; }
case "$action" in
  status) echo "Connected=$(connected)" ;;
  connect)
    sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 ConnectProfile s "$HFP_AG" >/dev/null
    echo "HFP ConnectProfile ok; Connected=$(connected)" ;;
  disconnect)
    sudo -n timeout 25 busctl call org.bluez "$device" org.bluez.Device1 DisconnectProfile s "$HFP_AG" >/dev/null || true
    echo "HFP DisconnectProfile sent; Connected=$(connected)" ;;
esac
