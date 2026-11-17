#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task-32 wired projection performance + soak measurement CLI.
#
#   aa-projection-perf.sh --mode wired --duration 30m --build-dir BUILD --out result.json
#   aa-projection-perf.sh --mode wired --duration 2h  --build-dir BUILD --out soak.json
#   aa-projection-perf.sh --mode wired --capture capture.json --out result.json
#   Optional approved model-only policy: --thermal-policy orin-nano-cv-unavailable/1
#   Optional X session: --display-env DISPLAY[,XAUTHORITY] (default :1)
#   aa-projection-perf.sh --readiness-only [--out readiness.json]
#   Every live stage needs --drive-smoke RESULT (or AA_DRIVE_SMOKE) and
#   --phone-serial SERIAL (or AA_PHONE_SERIAL): a passing
#   tools/wired-qa/aa_drive_smoke.py result from that same phone, attached on
#   --port, for the same binaries and the default configuration (#137).
#   Every live stage drives the moving-map workload and needs --media-uri URI
#   (optional --media-type, --media-component, --geo-uri): media for the whole
#   stage, Google Maps dragged once a second (tools/perf/aa_moving_map.py).
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
# A live stage is refused (BLOCKED unoptimized_build) unless --build-dir names
# the build that produced the launched binaries and its CMakeCache.txt records
# an optimized CMAKE_BUILD_TYPE (Release/RelWithDebInfo/MinSizeRel); an
# unoptimized pixel conversion on the GUI thread fails the A/V gate by itself.
# Every run records that build type, the build directory and whether
# --allow-unoptimized-build was used; a run that override admits is reported
# NOT_QUALIFYING: unoptimized_build and is never a PASS.
#
# The launched binaries must come from that build: a binary inside the build
# directory is accepted, a copy deployed elsewhere only when it is byte-identical
# to a binary that build produced; anything else is BLOCKED build_dir_mismatch.
# A cross build names the build tree on the host that built it: the host verifies
# its local CMakeCache.txt and derives each binary from the tree, and the target
# records that verification as host-side. The guard's own lines are kept in the
# run output whether the stage starts or is refused.
#
# Exit codes: 0 PASS, 1 FAIL (budget violation), 2 usage error,
#             3 BLOCKED (prerequisite or metric unavailable), 4 internal error.
#
# Live runs require the approved phone on the qualified port, the display
# connected (for the audio-sync/audio-latency budgets to be meaningful) and an
# installed receiver/display binary. Every ssh is wrapped in `timeout`.
#
# Environment overrides: AA_PERF_HOST (default jetson.local), AA_PERF_PORT
# (default 1-2.1), AA_BUILD_DIR (default for --build-dir), AA_DISPLAY and
# AA_XAUTHORITY (default for --display-env; the caller's DISPLAY/XAUTHORITY are
# honoured after those, and :1 is only the fallback).

set -euo pipefail

TOOL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PY="${PYTHON:-python3}"
REPORTER="$TOOL_DIR/aa_perf_report.py"

MODE=""
DURATION=""
DURATION_SET=0
HOST="${AA_PERF_HOST:-jetson.local}"
PORT="${AA_PERF_PORT:-1-2.1}"
RECEIVER="/opt/aa/aa-wired-receiver"
DISPLAY_BIN="/opt/aa/aa-dummy-display"
STORE=""
OUT=""
CAPTURE=""
CAPTURE_OUT=""
PROVENANCE=""
INTERVAL=5
NOW=""
READINESS_ONLY=0
THERMAL_POLICY=""
BUILD_DIR="${AA_BUILD_DIR:-}"
ALLOW_UNOPTIMIZED=0
DISPLAY_ENV="${AA_DISPLAY_ENV:-}"
DRIVE_SMOKE="${AA_DRIVE_SMOKE:-}"
PHONE_SERIAL="${AA_PHONE_SERIAL:-}"
MEDIA_URI="${AA_MEDIA_URI:-}"
MEDIA_TYPE="${AA_MEDIA_TYPE:-}"
MEDIA_COMPONENT="${AA_MEDIA_COMPONENT:-}"
GEO_URI="${AA_GEO_URI:-}"

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 -o ServerAliveCountMax=3)

usage() {
    sed -n '6,18p' "$0"
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
        --display) require_value "$@"; DISPLAY_BIN="$2"; shift 2 ;;
        --store) require_value "$@"; STORE="$2"; shift 2 ;;
        --out) require_value "$@"; OUT="$2"; shift 2 ;;
        --capture) require_value "$@"; CAPTURE="$2"; shift 2 ;;
        --capture-out) require_value "$@"; CAPTURE_OUT="$2"; shift 2 ;;
        --provenance) require_value "$@"; PROVENANCE="$2"; shift 2 ;;
        --interval) require_value "$@"; INTERVAL="$2"; shift 2 ;;
        --thermal-policy) require_value "$@"; THERMAL_POLICY="$2"; shift 2 ;;
        --build-dir) require_value "$@"; BUILD_DIR="$2"; shift 2 ;;
        --display-env) require_value "$@"; DISPLAY_ENV="$2"; shift 2 ;;
        --drive-smoke) require_value "$@"; DRIVE_SMOKE="$2"; shift 2 ;;
        --phone-serial) require_value "$@"; PHONE_SERIAL="$2"; shift 2 ;;
        --media-uri) require_value "$@"; MEDIA_URI="$2"; shift 2 ;;
        --media-type) require_value "$@"; MEDIA_TYPE="$2"; shift 2 ;;
        --media-component) require_value "$@"; MEDIA_COMPONENT="$2"; shift 2 ;;
        --geo-uri) require_value "$@"; GEO_URI="$2"; shift 2 ;;
        --allow-unoptimized-build) ALLOW_UNOPTIMIZED=1; shift ;;
        --now) require_value "$@"; NOW="$2"; shift 2 ;;
        --readiness-only) READINESS_ONLY=1; shift ;;
        -h | --help) usage ;;
        *) echo "usage error: unknown argument: $1" >&2; usage ;;
    esac
done

case "$THERMAL_POLICY" in
    "" | strict-all-zones | orin-nano-cv-unavailable/1) ;;
    *) echo "usage error: unknown --thermal-policy" >&2; exit 2 ;;
esac
thermal_args=()
if [[ -n "$THERMAL_POLICY" ]]; then
    thermal_args=(--thermal-policy "$THERMAL_POLICY")
fi

# X session for every remote command: --display-env wins, then the tool's own
# AA_DISPLAY/AA_XAUTHORITY, then the caller's DISPLAY/XAUTHORITY, and :1 is only
# the fallback. The qualified display is the display of the owner's session, so
# the script must never silently overrule a caller that names one. Both values
# are embedded in a remote shell command, so they are restricted to characters
# that need no quoting.
check_display_value() {
    local label="$1" value="$2"
    if [[ ! "$value" =~ ^[A-Za-z0-9:/._,@+-]+$ ]]; then
        echo "usage error: ${label} '${value}' is not a plain DISPLAY/XAUTHORITY value" >&2
        exit 2
    fi
}
if [[ -n "$DISPLAY_ENV" ]]; then
    DISPLAY_TARGET="${DISPLAY_ENV%%,*}"
    if [[ "$DISPLAY_ENV" == *,* ]]; then
        XAUTHORITY_TARGET="${DISPLAY_ENV#*,}"
    else
        XAUTHORITY_TARGET="${AA_XAUTHORITY:-${XAUTHORITY:-}}"
    fi
else
    DISPLAY_TARGET="${AA_DISPLAY:-${DISPLAY:-}}"
    XAUTHORITY_TARGET="${AA_XAUTHORITY:-${XAUTHORITY:-}}"
fi
DISPLAY_TARGET="${DISPLAY_TARGET:-:1}"
check_display_value "--display-env DISPLAY" "$DISPLAY_TARGET"
if [[ -n "$XAUTHORITY_TARGET" ]]; then
    check_display_value "--display-env XAUTHORITY" "$XAUTHORITY_TARGET"
fi

# Build guard arguments, forwarded verbatim into the on-target collector so the
# refusal happens before the receiver or display is launched there.
build_args=()
if [[ -n "$BUILD_DIR" ]]; then
    if [[ "$BUILD_DIR" == *"'"* ]]; then
        echo "usage error: --build-dir must not contain a single quote" >&2
        exit 2
    fi
    build_args+=(--build-dir "$BUILD_DIR")
fi
if ((ALLOW_UNOPTIMIZED)); then
    build_args+=(--allow-unoptimized-build)
fi
build_args_remote=""
if ((${#build_args[@]})); then
    printf -v build_args_remote ' %q' "${build_args[@]}"
fi

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
REMOTE_ENV='export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus";'
REMOTE_ENV+=" export DISPLAY=${DISPLAY_TARGET};"
if [[ -n "$XAUTHORITY_TARGET" ]]; then
    REMOTE_ENV+=" export XAUTHORITY=${XAUTHORITY_TARGET};"
fi

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
# gated metric is UNAVAILABLE) and return the reporter's exit code. The build
# provenance known at that point is recorded too (arguments: reason,
# build type, build directory, override used), so a blocked run still says which
# build it was about and whether the qualification override was in play.
blocked_result() {
    local reason="$1"
    local capture="$BUFFER/blocked-capture.json"
    AA_BLOCK_BUILD_TYPE="${2:-}" AA_BLOCK_BUILD_DIR="${3:-}" \
        AA_BLOCK_ALLOW_UNOPTIMIZED="${4:-0}" "$PY" -B - "$capture" <<'PY'
import json, os, sys
build_type = os.environ.get("AA_BLOCK_BUILD_TYPE") or None
build_dir = os.environ.get("AA_BLOCK_BUILD_DIR") or None
doc = {
    "schema": "aa-projection-perf-capture/3",
    "av_series": {"pair_t_ns": [], "pair_skew_ns": [], "audio_intervals_ns": []},
    "workload": {"kind": "none"},
    "monotonic_origin_ns": 0,
    "cpu_ticks_per_second": 1,
    "video_evidence": {"blocks": [], "ipc_latency_ns": [], "decode_latency_ns": []},
    "mode": "wired",
    "duration_seconds": 0,
    "crashes": 0,
    "samples": [],
    "events": {"helper_rss_mb": None},
    "build": {
        "build_type": build_type,
        "build_dir": build_dir,
        "allow_unoptimized_build": os.environ.get("AA_BLOCK_ALLOW_UNOPTIMIZED") == "1",
        "verified_on": "none",
    },
}
with open(sys.argv[1], "w", encoding="utf-8") as handle:
    json.dump(doc, handle)
PY
    "$PY" -B "$REPORTER" --live --capture "$capture" --out "$OUT" \
        --duration-requested "${DURATION:-}" --now "${NOW:-$(date -u +%Y-%m-%dT%H:%M:%SZ)}" \
        --blocked-on "$reason" >/dev/null 2>&1 || true
    return 3
}

# Each result is validated against its own version's schema (a v2 capture
# replays to a v2 result; live stages produce v3).
validate_result() {
    "$PY" -B "$TOOL_DIR/aa_perf_schema.py" validate-auto "$OUT" >/dev/null
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
        "${thermal_args[@]}" \
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
# The soak workload (#137): every live stage drags a moving map with media
# playing for the whole stage. The values reach the remote collector inside
# single quotes, so they may not contain one.
if [[ -z "$MEDIA_URI" ]]; then
    echo "usage error: --media-uri is required for a live stage (moving-map workload)" >&2
    exit 2
fi
for value in "$MEDIA_URI" "$MEDIA_TYPE" "$MEDIA_COMPONENT" "$GEO_URI"; do
    if [[ "$value" == *"'"* ]]; then
        echo "usage error: --media-uri/--media-type/--media-component/--geo-uri must not contain a single quote" >&2
        exit 2
    fi
done

if ! check_readiness; then
    echo "BLOCKED: ${ready_reason}" >&2
    blocked_result "$ready_reason" "" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
    exit 3
fi

# ---- build provenance (which build produced the binaries that will run) -----
# A build directory on the target is re-verified there, together with the
# binaries this run actually launches. A build directory only this host can see
# is the cross-build case: the host checks its own CMakeCache.txt and derives
# each launched binary from that tree before anything is launched remotely, and
# the target records that the check was host-side.
build_type_known=""
if [[ -n "$BUILD_DIR" ]]; then
    if remote "test -f '${BUILD_DIR}/CMakeCache.txt'" >/dev/null 2>&1; then
        : # target-side verification: the collector re-reads this cache
    elif [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
        guard_args=(--build-dir "$BUILD_DIR" --json --expect "receiver=$(basename "$RECEIVER")")
        if [[ -n "$DISPLAY_BIN" ]]; then
            guard_args+=(--expect "display=$(basename "$DISPLAY_BIN")")
        fi
        if ((ALLOW_UNOPTIMIZED)); then
            guard_args+=(--allow-unoptimized-build)
        fi
        if ! "$PY" -B "$TOOL_DIR/aa_build_guard.py" "${guard_args[@]}" \
                >"$BUFFER/build-check.json" 2>"$BUFFER/build-guard.err"; then
            if grep -q '^error: build_dir_mismatch:' "$BUFFER/build-guard.err"; then
                echo "BLOCKED: build_dir_mismatch" >&2
                sed -n '1,20p' "$BUFFER/build-guard.err" >&2 || true
                blocked_result "build_dir_mismatch" "" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
                exit 3
            fi
            echo "BLOCKED: unoptimized_build" >&2
            sed -n '1,20p' "$BUFFER/build-guard.err" >&2 || true
            blocked_result "unoptimized_build" "" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
            exit 3
        fi
        sed -n '1,20p' "$BUFFER/build-guard.err" >&2
        build_type_known="$("$PY" -c 'import json, sys; print(json.load(open(sys.argv[1]))["build_type"] or "")' \
            "$BUFFER/build-check.json")"
        if [[ -z "$build_type_known" ]]; then
            echo "internal error: the build guard produced no build type" >&2
            exit 4
        fi
        build_args_remote=" --build-dir '${BUILD_DIR}' --build-dir-verified-locally '${build_type_known}'"
        if ((ALLOW_UNOPTIMIZED)); then
            build_args_remote+=" --allow-unoptimized-build"
        fi
    fi
fi

# ---- drive smoke gate (#137) ---------------------------------------------
# No soak stage starts without a passing moving-map drive smoke on the same
# build: both launched binaries are hashed where they run and must match the
# smoke's. A binary that cannot be hashed (or no display at all) blocks the
# stage. Freshness (aa_drive_smoke.py RECORD_MAX_AGE_S) uses the real clock:
# --now only dates the result document and is never forwarded to the gate.
# The soak stages launch the receiver and display with default decode and
# presentation flags, so only a default-configuration smoke is accepted.
remote_sha256() {
    local path="$1" line=""
    [[ -n "$path" ]] || return 0
    line="$(remote "sha256sum -- '${path}'" 2>/dev/null)" || return 0
    awk 'NR == 1 {print $1}' <<<"$line"
}
receiver_sha="$(remote_sha256 "$RECEIVER")"
display_sha="$(remote_sha256 "$DISPLAY_BIN")"
# The smoke must come from the soak's own phone: --phone-serial must be attached
# (adb state "device") on --port of the target, or it is passed on empty and the
# gate blocks with drive_smoke_phone_unverified.
verified_serial=""
if [[ "$PHONE_SERIAL" =~ ^[A-Za-z0-9._:-]+$ ]]; then
    devices="$(remote "adb devices -l" 2>/dev/null || true)"
    if awk -v serial="$PHONE_SERIAL" -v port="usb:${PORT}" '
            $1 == serial && $2 == "device" { for (i = 3; i <= NF; i++) if ($i == port) found = 1 }
            END { exit found ? 0 : 1 }' <<<"$devices"; then
        verified_serial="$PHONE_SERIAL"
    fi
fi
if ! smoke_reason="$("$PY" -B "$TOOL_DIR/../wired-qa/aa_drive_smoke.py" check \
        --record "${DRIVE_SMOKE:-/nonexistent}" \
        --receiver-sha256 "$receiver_sha" --display-sha256 "$display_sha" \
        --phone-serial "$verified_serial")"; then
    smoke_reason="${smoke_reason:-drive_smoke_missing}"
    blocked_result "$smoke_reason" "$build_type_known" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
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
collector = (root / "aa_projection_collect.py").read_text()
print("import sys, types")
for name in ("aa_measurement_coverage", "aa_video_measurements", "aa_build_guard", "aa_moving_map"):
    source = (root / (name + ".py")).read_text()
    print("module = types.ModuleType(" + repr(name) + ")")
    print("sys.modules[module.__name__] = module")
    print("exec(" + repr(source) + ", module.__dict__)")
print("exec(compile(" + repr(collector) + ", 'aa_projection_collect.py', 'exec'))")
PY
collector_timeout=$((duration_secs + 120))
blocked_type=""
# A stop (TERM/INT, e.g. from tools/perf/aa_soak_chain.py) must reach the
# collector on the target: ending this side's ssh client does not signal a
# non-PTY remote command. The collector records its run in RUN_FILE on the
# target; on a stop, a separate ssh runs aa_remote_stop.sh, which TERMs it and
# waits for its acknowledgement (the run file removed after cleanup and phone
# restore), else KILLs the recorded processes and restores the phone itself.
run_id="$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
RUN_FILE="/tmp/aa-perf-run-${run_id}"
REMOTE_STOP_ACK_S="${AA_REMOTE_STOP_ACK_S:-30}"
stop_signal=""
trap 'stop_signal=TERM' TERM
trap 'stop_signal=INT' INT
timeout "$collector_timeout" ssh "${SSH_OPTS[@]}" "$HOST" "${REMOTE_ENV} python3 - --receiver '${RECEIVER}' --port '${PORT}' \
        --duration-seconds ${duration_secs} --interval ${INTERVAL} \
        ${THERMAL_POLICY:+--thermal-policy '${THERMAL_POLICY}'} \
        ${DISPLAY_BIN:+--display '${DISPLAY_BIN}'} ${STORE:+--store '${STORE}'}${build_args_remote} \
        ${verified_serial:+--phone-serial '${verified_serial}'} \
        --workload moving-map --media-uri '${MEDIA_URI}' ${MEDIA_TYPE:+--media-type '${MEDIA_TYPE}'} \
        ${MEDIA_COMPONENT:+--media-component '${MEDIA_COMPONENT}'} ${GEO_URI:+--geo-uri '${GEO_URI}'} \
        --run-file '${RUN_FILE}'" \
        <"$BUFFER/collector.py" >"$capture_file" 2>"$BUFFER/collector.err" &
collector_job=$!
collector_rc=0
while :; do
    set +e
    wait "$collector_job"
    collector_rc=$?
    set -e
    [[ -n "$stop_signal" ]] && break
    kill -0 "$collector_job" 2>/dev/null || break
done
if [[ -n "$stop_signal" ]]; then
    trap '' TERM INT  # the stop itself is not interrupted
    echo "stopping: ${stop_signal}; stopping the collector on ${HOST}" >&2
    stop_rc=0
    timeout $((REMOTE_STOP_ACK_S + 45)) ssh "${SSH_OPTS[@]}" "$HOST" \
        "bash -s -- '${RUN_FILE}' ${REMOTE_STOP_ACK_S}" <"$TOOL_DIR/aa_remote_stop.sh" >&2 || stop_rc=$?
    # rc 1: the stop finished but reported a problem (a survivor, or a restore
    # step such as the media stop that could not be done or verified).
    ((stop_rc == 0)) || echo "stopping: the remote stop reported a failure (rc ${stop_rc})" >&2
    # The remote command has ended (or was killed): drop the local client.
    kill -TERM "$collector_job" 2>/dev/null || true
    wait "$collector_job" 2>/dev/null || true
    [[ "$stop_signal" == INT ]] && exit 130
    exit 143
fi
trap - TERM INT
if ((collector_rc != 0)); then
    # The on-target collector refuses to start a stage from an unoptimized build
    # or from binaries another build produced; report each as its own BLOCKED
    # reason, never a generic failure.
    blocked_type="$(sed -n 's/.*CMAKE_BUILD_TYPE=\([A-Za-z0-9_+.-]*\).*/\1/p' "$BUFFER/collector.err" | head -1)"
    if grep -q '^error: build_dir_mismatch:' "$BUFFER/collector.err"; then
        echo "BLOCKED: build_dir_mismatch" >&2
        sed -n '1,20p' "$BUFFER/collector.err" >&2 || true
        blocked_result "build_dir_mismatch" "$blocked_type" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
        exit 3
    fi
    # The receiver saw another phone than the gate verified, at the first link
    # or a reconnect (the identity is bound where the port is used, #137).
    if grep -q '^error: phone_identity_mismatch:' "$BUFFER/collector.err"; then
        echo "BLOCKED: phone_identity_mismatch" >&2
        sed -n '1,20p' "$BUFFER/collector.err" >&2 || true
        blocked_result "phone_identity_mismatch" "$blocked_type" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
        exit 3
    fi
    # A required phone command (USB reset, wake, media or Maps launch) failed
    # before the stage started: the harness never set up the moving map, so
    # nothing was measured (#137).
    if grep -q '^error: phone_setup_failed:' "$BUFFER/collector.err"; then
        echo "BLOCKED: phone_setup_failed" >&2
        sed -n '1,20p' "$BUFFER/collector.err" >&2 || true
        blocked_result "phone_setup_failed" "$blocked_type" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
        exit 3
    fi
    if grep -q '^error: unoptimized_build:' "$BUFFER/collector.err"; then
        echo "BLOCKED: unoptimized_build" >&2
        sed -n '1,20p' "$BUFFER/collector.err" >&2 || true
        blocked_result "unoptimized_build" "$blocked_type" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
        exit 3
    fi
    echo "BLOCKED: collector_failed" >&2
    sed -n '1,20p' "$BUFFER/collector.err" >&2 || true
    blocked_result "collector_failed" "$blocked_type" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
    exit 3
fi
# The collector writes the build record it verified to stderr; keep those lines
# in the run output even when the stage started, so an override can never be
# silent in a green-looking log.
if [[ -s "$BUFFER/collector.err" ]]; then
    sed -n '1,40p' "$BUFFER/collector.err" >&2 || true
fi
if [[ ! -s "$capture_file" ]]; then
    echo "BLOCKED: collector_produced_no_capture" >&2
    blocked_result "collector_produced_no_capture" "$blocked_type" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"
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
