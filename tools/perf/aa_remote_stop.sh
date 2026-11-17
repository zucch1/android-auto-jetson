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
# plus "phone SERIAL" and "media_package PKG", and "group PGID SID" for the
# collector's own process group (the collector makes itself its leader, so its
# adb children share it; the ssh session's shell is not in it).
#
#  1. If the run file is not there yet, wait up to APPEAR_SECONDS (default 15)
#     for it: the stop may race a collector that is still starting.
#  2. TERM the collector. Its own handler stops the stimulus, the display and
#     the receiver, restores the phone and then removes the run file: that
#     removal, within ACK_SECONDS (default 30), is the acknowledgement.
#  3. Otherwise (no acknowledgement, or the collector is already gone without
#     one), the independent fallback: KILL the collector's whole process group
#     (its in-flight adb commands included) and the receiver's and display's
#     groups, wait until every one of those groups is empty, and only then
#     restore the phone directly (USB mode mtp, stop the media player). A
#     group is only signalled while it is still the recorded one: same PGID
#     and session, and, if a process leads it, the recorded leader (start time).
#
# Prints one line and exits 0 for "remote-stop: no-run", "remote-stop:
# acknowledged" or "remote-stop: forced". If a process of a killed group
# survives the KILL, it still restores the phone but prints "remote-stop:
# failed: survivors PID..." and exits 1.
set -u
set -f  # no globbing of /proc fields (the scan's own glob is under set +f below)

run_file="${1:?usage: aa_remote_stop.sh RUN_FILE [ACK_SECONDS] [APPEAR_SECONDS]}"
ack_seconds="${2:-30}"
appear_seconds="${3:-15}"

# The fallback's signal; tests set 0 (no signal) to model a process that
# survives the KILL.
kill_signal="${AA_REMOTE_STOP_SIGNAL:-KILL}"

# /proc reads use builtins only (no fork per process): a scan stays cheap.
STAT=()
read_stat() {  # read_stat PID: STAT=(STATE PPID PGRP SID ...) from field 3 on
    local line
    { read -r line < "/proc/$1/stat"; } 2>/dev/null || return 1
    # shellcheck disable=SC2206
    STAT=(${line##*) })
}
starttime() {
    read_stat "$1" || return 0
    [[ "${STAT[0]:-}" == Z ]] && return 0  # a zombie can do nothing more
    printf '%s' "${STAT[19]:-}"
}
same_process() { [[ "$1" =~ ^[0-9]+$ && -n "$2" && "$(starttime "$1")" == "$2" ]]; }
group_members() {  # live (non-zombie) PIDs in group PGID_Q and session SID_Q
    local path pid
    set +f
    local paths=(/proc/[0-9]*)
    set -f
    for path in "${paths[@]}"; do
        pid="${path#/proc/}"
        read_stat "$pid" || continue
        [[ "${STAT[0]:-}" != Z && "${STAT[2]:-}" == "$PGID_Q" && "${STAT[3]:-}" == "$SID_Q" ]] \
            && printf '%s\n' "$pid"
    done
}
# kill_group PGID SID LEADER_START: KILL the group if it is still the recorded
# one; record it for the wait. A live process leading PGID with another start
# time means the number was reused: never signal that.
killed_groups=()
kill_group() {
    local pgid="$1" sid="$2" leader_start="$3" members
    [[ "$pgid" =~ ^[0-9]+$ && "$sid" =~ ^[0-9]+$ && "$pgid" -gt 1 ]] || return 0
    PGID_Q="$pgid" SID_Q="$sid"
    members="$(group_members)"
    [[ -n "$members" ]] || return 0
    if grep -qx -- "$pgid" <<<"$members" && [[ "$(starttime "$pgid")" != "$leader_start" ]]; then
        return 0
    fi
    kill "-$kill_signal" -- "-$pgid" 2>/dev/null
    killed_groups+=("$pgid:$sid")
}
collector_gone() { ! same_process "$collector_pid" "$collector_start"; }
group_empty() { PGID_Q="$1" SID_Q="$2"; [[ -z "$(group_members)" ]]; }
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
# The collector's group (its adb children too), then the receiver's and the
# display's (each leads its own session: PGID = SID = PID).
kill_group "$(field group 2)" "$(field group 3)" "$collector_start"
if same_process "$collector_pid" "$collector_start"; then  # also without a group record
    kill "-$kill_signal" "$collector_pid" 2>/dev/null
fi
for role in receiver display; do
    pid="$(field "$role" 2)"
    kill_group "$pid" "$pid" "$(field "$role" 3)"
done
# Nothing of those groups may outlive the restore: wait until each is empty.
survivors=()
for group in "${killed_groups[@]}"; do
    if ! wait_until 5 group_empty "${group%%:*}" "${group##*:}"; then
        PGID_Q="${group%%:*}" SID_Q="${group##*:}"
        mapfile -t left < <(group_members)
        survivors+=("${left[@]}")
    fi
done
if same_process "$collector_pid" "$collector_start" \
        && ! wait_until 5 collector_gone; then
    survivors+=("$collector_pid")
fi
if [[ -n "$serial" ]]; then
    timeout 15 adb -s "$serial" shell svc usb setFunctions mtp >/dev/null 2>&1 || true
    if [[ -n "$package" ]]; then
        timeout 15 adb -s "$serial" shell am force-stop "$package" >/dev/null 2>&1 || true
    fi
fi
rm -f -- "$run_file"
if ((${#survivors[@]})); then
    echo "remote-stop: failed: survivors ${survivors[*]}"
    exit 1
fi
echo "remote-stop: forced"
exit 0
