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
# non-zero. Exit 0 means a projection was actually observed via the D-Bus
# contract - never a green smoke without evidence (misleading_success_output).
#
# Usage:
#   run-wired-live-qa.sh --receiver /opt/aa/aa-wired-receiver \
#       --display /opt/aa/aa-dummy-display --out /tmp/task-30-live-qa.json
#   run-wired-live-qa.sh --readiness-only     # bounded, read-only prerequisite check
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
OUT="/tmp/task-30-live-qa.json"
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

# Session env for every remote command (single choke point): batch ssh has no
# session bus address (`busctl --user` fails "Failed to get credentials") and
# the display consumer is a Qt GUI app that aborts under xcb without DISPLAY.
# Single-quoted so `$(id -u)` expands on the TARGET, never here.
REMOTE_SESSION_ENV='export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus; export DISPLAY=:1;'

# Bounded remote command (hung_commands class): 30 s hard ceiling per ssh.
remote() { timeout 30 ssh "${SSH_OPTS[@]}" "$HOST" "$REMOTE_SESSION_ENV" "$@"; }

json_escape() { printf '%s' "$1" | tr -d '\000' | sed 's/\\/\\\\/g; s/"/\\"/g'; }

write_result() {
    local status="$1" blocked_on="$2" projection="$3" video="$4" audio="$5" input="$6" budget_ms="$7"
    mkdir -p "$(dirname "$OUT")"
    cat > "$OUT" <<EOF
{
  "task": "30. Qualify wired end-to-end projection",
  "phase": "B (live device)",
  "host": "$(json_escape "$HOST")",
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
phone_id="$(remote "cat /sys/bus/usb/devices/${PORT}/idVendor 2>/dev/null | tr -d '\n'; echo -n ':'; cat /sys/bus/usb/devices/${PORT}/idProduct 2>/dev/null | tr -d '\n'" || true)"
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
# Exact-token DRM status match: `disconnected` must NOT read as `connected`.
display_connected="false"
for token in $display_status; do
    if [[ "$token" == "connected" ]]; then
        display_connected="true"
    fi
done
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
# and requests projection. Both run under a bounded ssh; logs are captured on the
# target under /tmp for later retrieval (redaction applied before export).
start_ms="$(date +%s%3N)"
remote "nohup ${RECEIVER} --port ${PORT} > /tmp/aa-wired-receiver.log 2>&1 & echo \$!" >/tmp/aa-recv.pid
remote "nohup ${DISPLAY} --test-only-autostart > /tmp/aa-wired-display.log 2>&1 & echo \$!" >/tmp/aa-disp.pid

projection="false"
deadline=$(( $(date +%s%3N) + PROJECTION_TIMEOUT_MS ))
while (( $(date +%s%3N) < deadline )); do
    state="$(remote "busctl --user call org.custom.AndroidAutoReceiver /org/custom/AndroidAutoReceiver org.custom.AndroidAutoReceiver1 GetState 2>/dev/null" || true)"
    if [[ "$state" == *"projecting"* ]]; then
        projection="true"
        break
    fi
    sleep 1
done
budget_ms=$(( $(date +%s%3N) - start_ms ))

# An ACTIVE phone session is a stronger claim than the D-Bus `projecting`
# string (the control service can reach `projecting` before a phone session is
# active). The receiver emits its `active` event only when the session state
# machine reaches State::active, so that is the session-activation signal.
session_active="false"
if [[ "$projection" == "true" ]]; then
    remote "grep -q '\"event\":\"active\"' /tmp/aa-wired-receiver.log && echo ok" >/dev/null 2>&1 && session_active="true"
fi

video="false"; audio="false"; input="false"
if [[ "$session_active" == "true" ]]; then
    remote "grep -q video_sent /tmp/aa-wired-receiver.log && echo ok" >/dev/null 2>&1 && video="true"
    remote "grep -q audio_chunks /tmp/aa-wired-receiver.log && echo ok" >/dev/null 2>&1 && audio="true"
    remote "grep -q input_forwarded /tmp/aa-wired-receiver.log && echo ok" >/dev/null 2>&1 && input="true"
fi

# Best-effort bounded teardown.
remote "kill \$(cat /tmp/aa-disp.pid) 2>/dev/null || true; kill \$(cat /tmp/aa-recv.pid) 2>/dev/null || true" || true

# Verdict taxonomy: a missing prerequisite is BLOCKED; the session running but
# failing to project / activate / deliver every observable is FAILED. PASS
# requires an ACTIVE session AND all three delivery paths observed.
if [[ "$projection" != "true" || "$session_active" != "true" ]]; then
    write_result "FAILED" "session_not_active" "$projection" "$video" "$audio" "$input" "$budget_ms"
    exit 4
fi
if [[ "$video" != "true" || "$audio" != "true" || "$input" != "true" ]]; then
    write_result "FAILED" "observable_missing" "$projection" "$video" "$audio" "$input" "$budget_ms"
    exit 4
fi
write_result "PASS" "" "$projection" "$video" "$audio" "$input" "$budget_ms"
