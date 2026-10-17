#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task-32 wired projection performance + soak measurement CLI.
#
#   aa-projection-perf.sh --mode wired --duration 30m --out result.json
#   aa-projection-perf.sh --mode wired --duration 2h  --out soak.json
#   aa-projection-perf.sh --mode wired --capture capture.json --out result.json
#   aa-projection-perf.sh --readiness-only [--out readiness.json]
#
# This tool measures CPU/RSS, thermal state, frame drops, decode/IPC timing,
# A/V sync and audio latency during wired projection on the qualified Jetson,
# then evaluates CONTRACTUAL budgets (receiver RSS <=250 MB, CPU <=50% of one
# core, A/V sync <=50 ms, audio latency <=200 ms, RSS growth <10%, zero crashes;
# helper RSS is recorded null). A budget miss FAILS the tool (exit 1); an
# unavailable gated metric BLOCKS it (exit 3) -- a green run without measured
# values is never a pass (misleading_success_output). Budgets are fixed in
# tools/perf/aa-projection-perf.schema.json and never weakened here.
#
# Exit codes: 0 PASS, 1 FAIL (budget violation), 2 usage error,
#             3 BLOCKED (prerequisite or metric unavailable), 4 internal error.
#
# Live runs require the approved phone on the qualified port, the display
# connected (for the audio-sync/audio-latency budgets to be meaningful) and an
# installed receiver/display binary. Every ssh is wrapped in `timeout`.
#
# Environment overrides: AA_PERF_HOST (default jetson.local), AA_PERF_PORT
# (default 1-2.1).

set -euo pipefail

TOOL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PYTHON:-python3}"
SCHEMA="$TOOL_DIR/aa-projection-perf.schema.json"
REPORTER="$TOOL_DIR/aa_perf_report.py"

MODE=""
DURATION=""
DURATION_SET=0
HOST="${AA_PERF_HOST:-jetson.local}"
PORT="${AA_PERF_PORT:-1-2.1}"
RECEIVER="/opt/aa/aa-wired-receiver"
DISPLAY="/opt/aa/aa-dummy-display"
STORE=""
OUT=""
CAPTURE=""
CAPTURE_OUT=""
PROVENANCE=""
INTERVAL=5
NOW=""
READINESS_ONLY=0

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=3)

usage() {
    sed -n '4,30p' "$0"
    exit 2
}

# Strict duration parse: <int>[smh]? (no unit = seconds). Never aborts on a bad
# value -- callers turn a non-zero return into a usage error (exit 2).
duration_seconds() {
    local value="$1"
    if [[ ! "$value" =~ ^([0-9]+)([smh]?)$ ]]; then
        return 1
    fi
    local number="${BASH_REMATCH[1]}"
    case "${BASH_REMATCH[2]}" in
        "" | s) printf '%s' "$number" ;;
        m) printf '%s' "$((number * 60))" ;;
        h) printf '%s' "$((number * 3600))" ;;
    esac
}

require_value() {
    if (($# < 2)); then
        echo "usage error: $1 requires a value" >&2
        usage
    fi
}

while (($# > 0)); do
    case "$1" in
        --mode) require_value "$@"; MODE="$2"; shift 2 ;;
        --duration) require_value "$@"; DURATION="$2"; DURATION_SET=1; shift 2 ;;
        --host) require_value "$@"; HOST="$2"; shift 2 ;;
        --port) require_value "$@"; PORT="$2"; shift 2 ;;
        --receiver) require_value "$@"; RECEIVER="$2"; shift 2 ;;
        --display) require_value "$@"; DISPLAY="$2"; shift 2 ;;
        --store) require_value "$@"; STORE="$2"; shift 2 ;;
        --out) require_value "$@"; OUT="$2"; shift 2 ;;
        --capture) require_value "$@"; CAPTURE="$2"; shift 2 ;;
        --capture-out) require_value "$@"; CAPTURE_OUT="$2"; shift 2 ;;
        --provenance) require_value "$@"; PROVENANCE="$2"; shift 2 ;;
        --interval) require_value "$@"; INTERVAL="$2"; shift 2 ;;
        --now) require_value "$@"; NOW="$2"; shift 2 ;;
        --readiness-only) READINESS_ONLY=1; shift ;;
        -h | --help) usage ;;
        *) echo "usage error: unknown argument: $1" >&2; usage ;;
    esac
done

if ((!READINESS_ONLY)) && [[ "$MODE" != "wired" ]]; then
    echo "usage error: --mode must be 'wired' (got '${MODE:-<unset>}')" >&2
    exit 2
fi
if [[ ! "$INTERVAL" =~ ^[0-9]+([.][0-9]+)?$ ]] || [[ "$INTERVAL" == "0" || "$INTERVAL" == "0.0" ]]; then
    echo "usage error: --interval must be a positive number" >&2
    exit 2
fi

BUFFER="$(mktemp -d "${TMPDIR:-/tmp}/aa-perf.XXXXXX")"
trap 'rm -rf "${BUFFER:?}"' EXIT

# User-session environment for every remote command: batch ssh carries no
# session, so `busctl --user` fails ("Failed to get credentials") and Qt
# consumers abort without DISPLAY. The export prologue (rather than a bare
# VAR= prefix) keeps compound remote commands working, and $(id -u) must
# expand on the target, never locally.
REMOTE_ENV='export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"; export DISPLAY=:1;'

remote() { timeout 30 ssh "${SSH_OPTS[@]}" "$HOST" "${REMOTE_ENV} $*"; }

# Bounded remote command with an explicit timeout (live collection is long).
remote_long() {
    local seconds="$1"
    shift
    timeout "$seconds" ssh "${SSH_OPTS[@]}" "$HOST" "${REMOTE_ENV} $*"
}

ready_reason=""
check_readiness() {
    if ! remote "true" >/dev/null 2>&1; then
        ready_reason="ssh_unreachable"
        return 1
    fi
    if ! remote "test -x '$RECEIVER'" >/dev/null 2>&1; then
        ready_reason="receiver_binary_absent"
        return 1
    fi
    local display_status
    display_status="$(remote "for c in /sys/class/drm/card*-*/status; do [ -f \"\$c\" ] && cat \"\$c\"; done; if command -v xrandr >/dev/null 2>&1; then if xrandr --query 2>/dev/null | grep -Eq '^[^[:space:]]+[[:space:]]+connected([[:space:]]|$)'; then printf 'connected\\n'; fi; fi" 2>/dev/null || true)"
    if ! grep -Fxq 'connected' <<<"$display_status"; then
        ready_reason="display_disconnected"
        return 1
    fi
    local audio_sink
    audio_sink="$(remote "sink=''; if command -v pactl >/dev/null 2>&1; then sink=\$(pactl list short sinks 2>/dev/null) || sink=''; fi; if [ -n \"\$sink\" ]; then printf '%s\\n' \"\$sink\"; elif command -v pw-cli >/dev/null 2>&1; then nodes=\$(pw-cli ls Node 2>/dev/null) || nodes=''; printf '%s\\n' \"\$nodes\" | grep -E '^[[:space:]]*media[.]class[[:space:]]*=[[:space:]]*\"?Audio/Sink\"?[[:space:]]*$'; fi" 2>/dev/null || true)"
    if [[ -z "$audio_sink" ]]; then
        ready_reason="no_audio_sink"
        return 1
    fi
    return 0
}

write_readiness() {
    local ready="$1" reason="$2"
    if [[ -z "$OUT" ]]; then
        return 0
    fi
    "$PY" -B - "$OUT" "$ready" "$reason" <<'PY'
import json, sys
out, ready, reason = sys.argv[1:4]
doc = {
    "schema": "aa-projection-perf-readiness/1",
    "ready": ready == "true",
    "blocked_on": reason or None,
}
with open(out, "w", encoding="utf-8") as handle:
    json.dump(doc, handle, indent=2, sort_keys=True)
    handle.write("\n")
PY
}

# Emit a schema-valid BLOCKED result via the reporter (no samples -> every
# gated metric is UNAVAILABLE) and return the reporter's exit code.
blocked_result() {
    local reason="$1"
    local capture="$BUFFER/blocked-capture.json"
    "$PY" -B - "$capture" <<'PY'
import json, sys
with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump({
        "schema": "aa-projection-perf-capture/1",
        "mode": "wired",
        "duration_seconds": 0,
        "crashes": 0,
        "samples": [],
        "events": {"helper_rss_mb": None},
    }, handle)
PY
    "$PY" -B "$REPORTER" --live --capture "$capture" --out "$OUT" \
        --duration-requested "${DURATION:-}" --now "${NOW:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}" \
        --blocked-on "$reason" >/dev/null 2>&1 || true
    return 3
}

validate_result() {
    "$PY" -B "$TOOL_DIR/aa_perf_schema.py" validate "$SCHEMA" "$OUT" >/dev/null
}

# ---- readiness-only --------------------------------------------------------
if ((READINESS_ONLY)); then
    if check_readiness; then
        write_readiness true ""
        echo "READY: ${HOST} prerequisites met (mode=wired, port=${PORT})"
        exit 0
    fi
    write_readiness false "$ready_reason"
    echo "BLOCKED: ${ready_reason}" >&2
    exit 3
fi

# ---- resolve duration ------------------------------------------------------
duration_secs=""
if ((DURATION_SET)); then
    if ! duration_secs="$(duration_seconds "$DURATION")"; then
        echo "usage error: invalid --duration '${DURATION}' (expected e.g. 30m, 2h, 90s)" >&2
        exit 2
    fi
fi

if [[ -z "$OUT" ]]; then
    echo "usage error: --out is required" >&2
    exit 2
fi

# ---- capture mode (deterministic test seam; no live measurement) -----------
if [[ -n "$CAPTURE" ]]; then
    if [[ ! -f "$CAPTURE" ]]; then
        echo "usage error: --capture file not found: $CAPTURE" >&2
        exit 2
    fi
    capture_sha="$(sha256sum "$CAPTURE" | cut -d' ' -f1)"
    set +e
    "$PY" -B "$REPORTER" --capture "$CAPTURE" --out "$OUT" \
        --duration-requested "${DURATION:-}" --capture-sha256 "$capture_sha" \
        --now "${NOW:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}"
    rc=$?
    set -e
    if [[ "$rc" == "4" ]]; then
        echo "internal error: reporter rejected the capture (contract violation)" >&2
        exit 4
    fi
    if [[ "$rc" == "2" ]]; then
        exit 2
    fi
    if ! validate_result; then
        echo "internal error: result document failed schema validation" >&2
        exit 4
    fi
    exit "$rc"
fi

# ---- live run --------------------------------------------------------------
if [[ -z "$duration_secs" ]]; then
    echo "usage error: --duration is required for a live run" >&2
    exit 2
fi

if ! check_readiness; then
    echo "BLOCKED: ${ready_reason}" >&2
    blocked_result "$ready_reason"
    exit 3
fi

# Provenance: computed locally where possible, bounded remote hashes otherwise.
prov_file="$BUFFER/provenance.json"
if [[ -n "$PROVENANCE" ]]; then
    cp "$PROVENANCE" "$prov_file"
fi

# Collect a capture on the target via a bounded ssh (collector piped on stdin).
capture_file="$BUFFER/capture.json"
"$PY" -B - "$TOOL_DIR" >"$BUFFER/collector.py" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
source = (root / "aa_video_measurements.py").read_text()
collector = (root / "aa_projection_collect.py").read_text()
print("import sys, types")
print("module = types.ModuleType('aa_video_measurements')")
print("sys.modules[module.__name__] = module")
print("exec(" + repr(source) + ", module.__dict__)")
print("exec(compile(" + repr(collector) + ", 'aa_projection_collect.py', 'exec'))")
PY
collector_timeout=$((duration_secs + 120))
if ! remote_long "$collector_timeout" "python3 - --receiver '${RECEIVER}' --port '${PORT}' \
        --duration-seconds ${duration_secs} --interval ${INTERVAL} \
        ${DISPLAY:+--display '${DISPLAY}'} ${STORE:+--store '${STORE}'}" \
        <"$BUFFER/collector.py" >"$capture_file" 2>"$BUFFER/collector.err"; then
    echo "BLOCKED: collector_failed" >&2
    sed -n '1,20p' "$BUFFER/collector.err" >&2 || true
    blocked_result "collector_failed"
    exit 3
fi
if [[ ! -s "$capture_file" ]]; then
    echo "BLOCKED: collector_produced_no_capture" >&2
    blocked_result "collector_produced_no_capture"
    exit 3
fi
if [[ -n "$CAPTURE_OUT" ]]; then
    cp "$capture_file" "$CAPTURE_OUT"
fi
capture_sha="$(sha256sum "$capture_file" | cut -d' ' -f1)"

report_args=(--live --capture "$capture_file" --out "$OUT" --duration-requested "$DURATION"
    --capture-sha256 "$capture_sha" --now "${NOW:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}")
if [[ -s "$prov_file" ]]; then
    report_args+=(--provenance "$prov_file")
fi
set +e
"$PY" -B "$REPORTER" "${report_args[@]}"
rc=$?
set -e
if [[ "$rc" == "4" ]]; then
    echo "internal error: reporter rejected the capture (contract violation)" >&2
    exit 4
fi
if [[ "$rc" == "2" ]]; then
    exit 2
fi
if ! validate_result; then
    echo "internal error: result document failed schema validation" >&2
    exit 4
fi
exit "$rc"
