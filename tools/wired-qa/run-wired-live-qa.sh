#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task 30 phase-B wired live-QA harness (READY TO RUN; deliberately NOT run in
# phase A - the Jetson display is disconnected). It qualifies the REAL approved
# phone over USB/AOA on jetson.local and drives the receiver -> dummy display ->
# audio/input path, capturing evidence and budget timing into a JSON result.
#
# Fail-closed by construction: every ssh runs under `timeout`; a missing
# prerequisite (phone absent, udev policy not installed, no audio sink, display
# disconnected, receiver binary not supplied) writes a BLOCKED result and exits
# non-zero. Exit 0 requires projection plus painted frames, a successful audio
# pump and delivered input - never a green smoke without delivery evidence.
#
# Usage:
#   run-wired-live-qa.sh --receiver /opt/aa/aa-wired-receiver \
#       --display /opt/aa/aa-dummy-display --out /run/user/$UID/task-30-live-qa.json
#   run-wired-live-qa.sh --readiness-only     # bounded, read-only prerequisite check
# Without --out, the report is written in a private mktemp directory below
# $XDG_RUNTIME_DIR. Target-side logs and the control FIFO use a separate private
# directory below the target's XDG_RUNTIME_DIR.
#
# The receiver is tools/wired-receiver (target aa-wired-receiver): it runs the
# wired session over USB/AOA with TrustStore admission and serves the video /
# input / D-Bus surfaces the dummy display registers against. Its CLI is
#   aa-wired-receiver --port <usb-port-context> [--store PATH] [--no-bus]
#
# Environment overrides: HOST (default jetson.local), PORT (default 1-2.1).

set -euo pipefail

HOST="${HOST:-jetson.local}"
PORT="${PORT:-1-2.1}"
RECEIVER=""
DISPLAY=""
OUT=""
PROJECTION_TIMEOUT_MS=15000
READINESS_ONLY=0
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)

usage() {
    sed -n '2,28p' "$0"
    exit 2
}

while (($# > 0)); do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --receiver) RECEIVER="$2"; shift 2 ;;
        --display) DISPLAY="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --projection-timeout-ms) PROJECTION_TIMEOUT_MS="$2"; shift 2 ;;
        --readiness-only) READINESS_ONLY=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

if [[ -z "$OUT" ]]; then
    if [[ -z "${XDG_RUNTIME_DIR:-}" || ! -d "$XDG_RUNTIME_DIR" ]]; then
        echo "XDG_RUNTIME_DIR must name an existing directory when --out is omitted" >&2
        exit 2
    fi
    QA_WORK_DIR="$(mktemp -d -p "$XDG_RUNTIME_DIR" aa-wired-live-qa.XXXXXX)"
    chmod 0700 "$QA_WORK_DIR"
    OUT="$QA_WORK_DIR/result.json"
fi

# Session env for every remote command (single choke point): batch ssh has no
# session bus address (`busctl --user` fails "Failed to get credentials") and
# the display consumer is a Qt GUI app that aborts under xcb without DISPLAY.
# Single-quoted so `$(id -u)` expands on the TARGET, never here.
# XAUTHORITY requirement: DISPLAY alone is not enough for the X client - the
# remote session must also carry XAUTHORITY pointing at the authority cookie
# of the target X server (the owning session's cookie file), or the display
# consumer is refused with an X authentication failure. Set both together.
REMOTE_SESSION_ENV='export XDG_RUNTIME_DIR=/run/user/$(id -u); export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus; export DISPLAY=:1;'

# Shell-quote one caller-supplied value before it enters a remote command
# string: remote() runs that string through the TARGET shell, so a
# metacharacter in PORT/RECEIVER/DISPLAY must arrive literal, never execute.
shq() { printf '%q' "$1"; }

# Bounded remote command (hung_commands class): 30 s hard ceiling per ssh.
remote() { timeout 30 ssh "${SSH_OPTS[@]}" "$HOST" "$REMOTE_SESSION_ENV" "$@"; }

json_escape() { printf '%s' "$1" | tr -d '\000' | sed 's/\\/\\\\/g; s/"/\\"/g'; }

# Host/IP values are never written raw to a JSON artifact: keep only a
# truncated digest for run correlation (Privacy: redact/hash before export).
host_tag="$(printf '%s' "$HOST" | sha256sum | cut -c1-16)"

write_result() {
    local status="$1" blocked_on="$2" projection="$3" video="$4" audio="$5" input="$6" budget_ms="$7"
    # Pin the checked parent directory, then create once without following links.
    # An existing file (including a dangling symlink) is never overwritten.
    python3 -c '
import os
import stat
import sys

path = sys.argv[1]
try:
    parent, name = os.path.split(os.path.abspath(path))
    directory = os.open(parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        if os.fstat(directory).st_mode & stat.S_IWOTH:
            raise OSError("parent directory is world-writable; use a private directory")
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                     0o600, dir_fd=directory)
        with os.fdopen(fd, "w") as output:
            os.fchmod(output.fileno(), 0o600)
            output.write(sys.stdin.read())
    finally:
        os.close(directory)
except OSError as error:
    print(f"Refusing report output {path!r}: {error}", file=sys.stderr)
    sys.exit(2)
' "$OUT" <<EOF
{
  "task": "30. Qualify wired end-to-end projection",
  "phase": "B (live device)",
  "host": "sha256:${host_tag}",
  "port": "$(json_escape "$PORT")",
  "status": "$status",
  "blocked_on": "$(json_escape "$blocked_on")",
  "observables": {
    "projection_reached": $projection,
    "video_present": $video,
    "audio_present": $audio,
    "input_evidence": $input,
    "startup_budget_ms": $budget_ms
  },
  "note": "phase-A host-side acceptance is separate; this is the live record"
}
EOF
    echo "wrote $OUT (status=$status${blocked_on:+ blocked_on=$blocked_on})"
}

# ---- 1. reachability (bounded) ----
if ! remote "true" >/dev/null 2>&1; then
    write_result "BLOCKED" "ssh_unreachable" "false" "false" "false" "false" "null"
    echo "BLOCKED: $HOST not reachable over ssh" >&2
    exit 3
fi

# ---- 2. read-only readiness checks ----
PORT_Q="$(shq "$PORT")"
phone_id="$(remote "cat /sys/bus/usb/devices/${PORT_Q}/idVendor 2>/dev/null | tr -d '\n'; echo -n ':'; cat /sys/bus/usb/devices/${PORT_Q}/idProduct 2>/dev/null | tr -d '\n'" || true)"
# DRM connector status tokens only (no card name): one word per connector.
display_status="$(remote "for c in /sys/class/drm/card*-DP-1/status /sys/class/drm/card*-HDMI-A-1/status; do [ -f \"\$c\" ] && cat \"\$c\"; done" || true)"
audio_sink="$(remote "command -v pactl >/dev/null 2>&1 && pactl list short sinks 2>/dev/null | head -1 || (command -v pw-cli >/dev/null 2>&1 && pw-cli ls Device 2>/dev/null | head -1)" || true)"
policy="$(remote "test -f /etc/udev/rules.d/70-android-auto-receiver.rules && echo installed || echo absent" || true)"

echo "readiness: phone_id='$phone_id' policy='$policy'" >&2
echo "readiness: display='$display_status'" >&2
echo "readiness: audio_sink='$audio_sink'" >&2

if [[ -z "$phone_id" || "$phone_id" == ":" ]]; then
    write_result "BLOCKED" "phone_absent_on_port" "false" "false" "false" "false" "null"
    exit 3
fi
if [[ "$policy" != "installed" ]]; then
    write_result "BLOCKED" "udev_policy_not_installed" "false" "false" "false" "false" "null"
    exit 3
fi
# Exact-line DRM status record (lane-32 pattern, tools/perf/aa-projection-perf.sh):
# `disconnected` must NOT read as `connected`; only an exact `connected` line passes.
display_connected="false"
if printf '%s\n' "$display_status" | grep -Fxq 'connected'; then
    display_connected="true"
fi
# NVIDIA DRM can report disconnected while X drives a live monitor. Accept only
# a successful query with an exact second-field `connected` token, parsed locally.
if [[ "$display_connected" != "true" ]]; then
    if xrandr_status="$(remote "xrandr --query 2>/dev/null")"; then
        if printf '%s\n' "$xrandr_status" | grep -Eq '^[^ ]+ connected( |$)'; then
            display_connected="true"
        fi
    fi
fi
if [[ "$display_connected" != "true" ]]; then
    write_result "BLOCKED" "display_disconnected" "false" "false" "false" "false" "null"
    exit 3
fi
if [[ -z "$audio_sink" ]]; then
    write_result "BLOCKED" "no_audio_sink" "false" "false" "false" "false" "null"
    exit 3
fi
if ((READINESS_ONLY)); then
    write_result "READY" "" "false" "false" "false" "false" "null"
    exit 0
fi

# ---- 3. live projection (requires the on-target wired receiver + display) ----
if [[ -z "$RECEIVER" || -z "$DISPLAY" ]]; then
    write_result "BLOCKED" "receiver_or_display_binary_not_supplied" "false" "false" "false" "false" "null"
    echo "BLOCKED: --receiver and --display are required for the projection run" >&2
    exit 3
fi

# The receiver owns the D-Bus contract name; the display registers as a consumer
# and requests projection. Both run under a bounded ssh; logs and the control
# FIFO live in a fresh mode-0700 target directory, never predictable /tmp paths.
# The display uses the shipped TEST-ONLY stdin control channel
# (--test-only-control-stdin) so the harness can request real status (painted
# frames) and stimulate one input event (gate P2-G).
REMOTE_QA_DIR="$(remote 'mktemp -d -p "$XDG_RUNTIME_DIR" aa-wired-live-qa.XXXXXX')" || {
    write_result "BLOCKED" "target_private_workdir_unavailable" "false" "false" "false" "false" "null"
    exit 3
}
if [[ -z "$REMOTE_QA_DIR" || "$REMOTE_QA_DIR" != /* || "$REMOTE_QA_DIR" == *$'\n'* ]]; then
    write_result "BLOCKED" "target_private_workdir_invalid" "false" "false" "false" "false" "null"
    exit 3
fi
REMOTE_QA_DIR_Q="$(shq "$REMOTE_QA_DIR")"
start_ms="$(date +%s%3N)"
receiver_pid=""; display_pid=""
teardown() {
    local pid
    for pid in "$display_pid" "$receiver_pid"; do
        [[ -z "$pid" ]] && continue
        [[ "$pid" =~ ^[1-9][0-9]*$ ]] || return 1
        remote "kill $pid 2>/dev/null || true; for n in \$(seq 1 20); do kill -0 $pid 2>/dev/null || exit 0; sleep 0.1; done; kill -KILL $pid 2>/dev/null || true; sleep 0.1; ! kill -0 $pid 2>/dev/null" || return 1
    done
    receiver_pid=""; display_pid=""
}
on_exit() {
    local code=$?
    trap - EXIT
    teardown || code=4
    exit "$code"
}
trap on_exit EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
receiver_pid="$(remote "nohup $(shq "$RECEIVER") --port ${PORT_Q} > ${REMOTE_QA_DIR_Q}/aa-wired-receiver.log 2>&1 & echo \$!")"
# Open the FIFO read/write so startup does not wait for a separate writer and
# the test-only channel stays open between bounded status/input requests.
display_pid="$(remote "mkfifo ${REMOTE_QA_DIR_Q}/aa-disp.in && { nohup $(shq "$DISPLAY") --test-only-autostart --test-only-control-stdin <>${REMOTE_QA_DIR_Q}/aa-disp.in > ${REMOTE_QA_DIR_Q}/aa-wired-display.log 2>&1 & echo \$!; }")"

projection="false"
session_active="false"; video="false"; audio="false"; input="false"
deadline=$(( start_ms + PROJECTION_TIMEOUT_MS ))
poll_remote() {
    local remaining=$((deadline - $(date +%s%3N)))
    ((remaining > 0)) || return 1
    timeout "$(printf '%d.%03d' "$((remaining / 1000))" "$((remaining % 1000))")" \
        ssh "${SSH_OPTS[@]}" "$HOST" "$REMOTE_SESSION_ENV" "$@"
}
while (( $(date +%s%3N) < deadline )); do
    state="$(poll_remote "busctl --user call org.custom.AndroidAutoReceiver /org/custom/AndroidAutoReceiver org.custom.AndroidAutoReceiver1 GetState 2>/dev/null" || true)"
    if [[ "$state" == *"projecting"* ]]; then
        projection="true"
    fi
    poll_remote "grep -q '\"event\":\"active\"' ${REMOTE_QA_DIR_Q}/aa-wired-receiver.log" >/dev/null 2>&1 && session_active="true"
    if poll_remote "grep -q '\"event\":\"video_sent\"' ${REMOTE_QA_DIR_Q}/aa-wired-receiver.log" >/dev/null 2>&1; then
        status="$(poll_remote "printf 'status\n' > ${REMOTE_QA_DIR_Q}/aa-disp.in 2>/dev/null; grep '\"event\":\"status\"' ${REMOTE_QA_DIR_Q}/aa-wired-display.log 2>/dev/null | tail -1" || true)"
        if [[ "$status" =~ \"frames_painted\":([0-9]+) ]] && ((BASH_REMATCH[1] > 0)) && [[ "$status" == *'"render_ok":true'* ]]; then
            video="true"
        fi
    fi
    poll_remote "grep -q '\"event\":\"audio_chunks\"' ${REMOTE_QA_DIR_Q}/aa-wired-receiver.log && grep -q '\"event\":\"media_timing\"' ${REMOTE_QA_DIR_Q}/aa-wired-receiver.log" >/dev/null 2>&1 && audio="true"
    if [[ "$session_active" == true && "$input" != true ]]; then
        poll_remote "printf 'key a press\nkey a release\n' > ${REMOTE_QA_DIR_Q}/aa-disp.in 2>/dev/null" >/dev/null 2>&1 || true
    fi
    poll_remote "grep -q '\"event\":\"input_forwarded\",\"ok\":\"true\"' ${REMOTE_QA_DIR_Q}/aa-wired-receiver.log" >/dev/null 2>&1 && input="true"
    if [[ "$projection" == true && "$session_active" == true && "$video" == true && "$audio" == true && "$input" == true ]]; then
        break
    fi
    sleep 0.05
done
budget_ms=$(( $(date +%s%3N) - start_ms ))

# An ACTIVE phone session is a stronger claim than the D-Bus `projecting`
# string (the control service can reach `projecting` before a phone session is
# active). The receiver emits its `active` event only when the session state
# machine reaches State::active, so that is the session-activation signal.
if ! teardown; then
    write_result "FAILED" "teardown_failed" "$projection" "$video" "$audio" "$input" "$budget_ms"
    exit 4
fi

# Verdict taxonomy: a missing prerequisite is BLOCKED; the session running but
# failing to project / activate / deliver every observable is FAILED. PASS
# requires an ACTIVE session AND all three delivery paths observed: painted
# frames with render_ok, successful-pump audio_chunks plus media_timing, and
# input_forwarded ok:true. Final media_timing alone can follow a failed pump.
if [[ "$projection" != "true" || "$session_active" != "true" ]]; then
    write_result "FAILED" "session_not_active" "$projection" "$video" "$audio" "$input" "$budget_ms"
    exit 4
fi
if [[ "$video" != "true" || "$audio" != "true" || "$input" != "true" ]]; then
    write_result "FAILED" "observable_missing" "$projection" "$video" "$audio" "$input" "$budget_ms"
    exit 4
fi
write_result "PASS" "" "$projection" "$video" "$audio" "$input" "$budget_ms"
