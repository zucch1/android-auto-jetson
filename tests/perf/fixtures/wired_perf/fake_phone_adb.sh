#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fake `adb` for the moving-map cleanup tests (#137): records every argv in
# FAKE_ADB_LOG and models the parts of the phone the restore depends on.
#
#   media player   `am start ... -n PKG/ACT` starts PKG; `am force-stop PKG`
#                  stops it; `pidof PKG` prints a PID (exit 0) while it runs,
#                  nothing (exit 1) once it is gone, as Android's toybox does.
#   USB            `svc usb setFunctions ...` re-enumerates USB: every command
#                  for FAKE_ADB_REENUM_MS (default 300) after it fails with
#                  "device not found" (exit 1), as on the A26 right after the
#                  switch (a setup waits seconds after its own reset).
#
# Knobs:
#   FAKE_ADB_STUCK=PKG        force-stop never stops PKG (it re-spawns)
#   FAKE_ADB_RESPAWN=PKG:N    PKG re-spawns after each of its first N force-stops
#   FAKE_ADB_FAIL=NEEDLE[:N]  the Nth and later commands containing NEEDLE fail
#                             like an am start that cannot start its activity
#   FAKE_ADB_SLOW=FILE        a media launch (fixture.opus) writes its PID to
#                             FILE and stays in flight for 4 s before landing
#   FAKE_ADB_DEVICE_LINE      the `devices -l` line (default FAKESERIAL on 9-9)
#   FAKE_ADB_STALL_FILE=FILE  while FILE exists, every command is logged and
#                             then hangs (until the caller's timeout kills it)
#   FAKE_ADB_USB_CONFIG       `getprop sys.usb.config` (default mtp,adb)
#   FAKE_ADB_WIFI_STATUS      `cmd wifi status` text; FAKE_ADB_BT_ON bluetooth_on (1)
#   FAKE_ADB_CONNECT=fail     `adb connect IP:5555` fails (adb unreachable over the AP)
#   FAKE_ADB_SERIALNO         `get-serialno` answer (default FAKESERIAL)
#   FAKE_ADB_IGNORE_TERM=1    such a hung command also ignores TERM (only KILL
#                             ends it)
set -u
log="${FAKE_ADB_LOG:?}"
state="${FAKE_PHONE_STATE:-${log}.phone}"
mkdir -p "$state"
args="$*"
case "$args" in *fixture.opus*)
    if [ -n "${FAKE_ADB_SLOW:-}" ]; then echo "$$" > "$FAKE_ADB_SLOW"; sleep 4; fi ;;
esac
printf '%s\n' "$args" >> "$log"
if [ -n "${FAKE_ADB_STALL_FILE:-}" ] && [ -e "$FAKE_ADB_STALL_FILE" ]; then
    # An ignored TERM survives the exec: the sleep itself ignores it.
    [ -n "${FAKE_ADB_IGNORE_TERM:-}" ] && trap '' TERM
    exec sleep 600
fi

now_ms=$(( $(date +%s%N) / 1000000 ))
if [ -f "$state/reenumerating" ] && [ "$now_ms" -lt "$(cat "$state/reenumerating")" ]; then
    echo "adb: device 'FAKESERIAL' not found" >&2
    exit 1
fi
if [ -n "${FAKE_ADB_FAIL:-}" ]; then
    needle="${FAKE_ADB_FAIL%%:*}"; nth="${FAKE_ADB_FAIL#*:}"; [ "$nth" = "$FAKE_ADB_FAIL" ] && nth=1
    case "$args" in *"$needle"*)
        seen="$(grep -c -- "$needle" "$log")"
        if [ "$seen" -ge "$nth" ]; then echo "Error: Activity not started"; exit 1; fi ;;
    esac
fi
case "$args" in
    "devices -l")
        printf 'List of devices attached\n%s\n' \
            "${FAKE_ADB_DEVICE_LINE:-FAKESERIAL device usb:9-9 model:FAKE}" ;;
    "connect "*)
        # adb over the AP link (wireless): FAKE_ADB_CONNECT=fail refuses it.
        if [ "${FAKE_ADB_CONNECT:-ok}" = fail ]; then echo "failed to connect to ${args#connect }"; exit 1; fi
        echo "connected to ${args#connect }" ;;
    "disconnect "*)
        echo "disconnected ${args#disconnect }" ;;
    *"get-serialno"*)
        echo "${FAKE_ADB_SERIALNO:-FAKESERIAL}" ;;
    *"getprop sys.usb.config"*)
        echo "${FAKE_ADB_USB_CONFIG-mtp,adb}" ;;
    *"cmd wifi status"*)
        printf '%s\n' "${FAKE_ADB_WIFI_STATUS:-Wifi is connected to \"x\"}" ;;
    *"settings get global bluetooth_on"*)
        echo "${FAKE_ADB_BT_ON:-1}" ;;
    *"svc usb setFunctions"*)
        echo $(( now_ms + ${FAKE_ADB_REENUM_MS:-300} )) > "$state/reenumerating" ;;
    *"am start"*" -n "*)
        component="${args##* -n }"
        touch "$state/running.${component%%/*}" ;;
    *"am force-stop "*)
        package="${args##*force-stop }"
        stops="$(cat "$state/stops.$package" 2>/dev/null || echo 0)"
        echo $((stops + 1)) > "$state/stops.$package"
        respawns="${FAKE_ADB_RESPAWN:-}"
        if [ "${FAKE_ADB_STUCK:-}" = "$package" ]; then
            :
        elif [ "${respawns%%:*}" = "$package" ] && [ "$stops" -lt "${respawns#*:}" ]; then
            :
        else
            rm -f "$state/running.$package"
        fi ;;
    *"pidof "*)
        package="${args##*pidof }"
        if [ -f "$state/running.$package" ]; then echo 20941; exit 0; fi
        exit 1 ;;
esac
exit 0
