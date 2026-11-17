#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stop one on-target collector run and confirm its cleanup (#137).
#
#   bash -s -- RUN_FILE [ACK_SECONDS] [APPEAR_SECONDS] < aa_remote_stop.sh
#
# The launcher (tools/perf/aa-projection-perf.sh) runs this on the target over
# its own ssh call when it is stopped. Ending the local ssh client never
# signals a non-PTY remote command, so the stop must be explicit. RUN_FILE is
# the collector's --run-file: "ROLE PID START" for the collector, receiver and
# display (START is the /proc start time, so a reused PID is never signalled),
# plus "phone SERIAL" and "media_package PKG".
#
#  1. If the run file is not there yet, wait up to APPEAR_SECONDS (default 15)
#     for it: the stop may race a collector that is still starting.
#  2. TERM the collector. Its own handler stops the stimulus, the display and
#     the receiver, restores the phone and then removes the run file: that
#     removal, within ACK_SECONDS (default 30), is the acknowledgement.
#  3. Otherwise (no acknowledgement, or the collector is already gone without
#     one), the independent fallback: KILL every recorded process that is
#     still the one recorded (the receiver and display by process group), then
#     restore the phone directly (USB mode mtp, stop the media player).
#
# Prints one line, "remote-stop: no-run", "remote-stop: acknowledged" or
# "remote-stop: forced", and exits 0. It exits 1 if a recorded process
# survived the fallback.
set -u

run_file="${1:?usage: aa_remote_stop.sh RUN_FILE [ACK_SECONDS] [APPEAR_SECONDS]}"
ack_seconds="${2:-30}"
appear_seconds="${3:-15}"

starttime() {
    local stat
    stat="$(cat "/proc/$1/stat" 2>/dev/null)" || return 0
    stat="${stat##*) }"
    # shellcheck disable=SC2086
    set -- $stat
    printf '%s' "${20:-}"
}
same_process() { [[ "$1" =~ ^[0-9]+$ && -n "$2" && "$(starttime "$1")" == "$2" ]]; }
field() { awk -v key="$1" -v n="$2" '$1 == key { print $n; exit }' "$run_file" 2>/dev/null; }
wait_until() {  # wait_until SECONDS COMMAND...: poll every 0.1 s
    local ticks=$(( $1 * 10 ))
    shift
    while ((ticks-- > 0)); do
        "$@" && return 0
        sleep 0.1
    done
    "$@"
}
gone() { [[ ! -e "$run_file" ]]; }
exited() { ! same_process "$1" "$2"; }
present() { [[ -e "$run_file" ]]; }

if ! wait_until "$appear_seconds" present; then
    echo "remote-stop: no-run"
    exit 0
fi

collector_pid="$(field collector 2)"
collector_start="$(field collector 3)"
if same_process "$collector_pid" "$collector_start"; then
    kill -TERM "$collector_pid" 2>/dev/null
    if wait_until "$ack_seconds" gone; then
        echo "remote-stop: acknowledged"
        exit 0
    fi
fi

# Fallback: the collector did not confirm its cleanup in time.
serial="$(field phone 2)"
package="$(field media_package 2)"
survivors=0
for role in collector receiver display; do
    pid="$(field "$role" 2)"
    start="$(field "$role" 3)"
    same_process "$pid" "$start" || continue
    if [[ "$role" == collector ]]; then
        kill -KILL "$pid" 2>/dev/null
    else
        kill -KILL -- "-$pid" 2>/dev/null || kill -KILL "$pid" 2>/dev/null
    fi
done
for role in collector receiver display; do
    pid="$(field "$role" 2)"
    start="$(field "$role" 3)"
    same_process "$pid" "$start" || continue
    wait_until 5 exited "$pid" "$start" || survivors=1
done
if [[ -n "$serial" ]]; then
    timeout 15 adb -s "$serial" shell svc usb setFunctions mtp >/dev/null 2>&1 || true
    if [[ -n "$package" ]]; then
        timeout 15 adb -s "$serial" shell am force-stop "$package" >/dev/null 2>&1 || true
    fi
fi
rm -f -- "$run_file"
echo "remote-stop: forced"
exit "$survivors"
