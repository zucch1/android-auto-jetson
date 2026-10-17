#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task 31 phase-B wired reconnect/recovery live-QA harness (READY TO RUN). This
# is the live companion to the host-side logic matrix (`ctest -R wired_recovery`):
# it drives the REAL approved phone + on-target wired receiver through the plan's
# recovery scenarios and records per-scenario observed evidence, recovery time
# and retry counts.
#
# It is deliberately NOT run in phase A: the Jetson display is DP-1 DISCONNECTED,
# the on-target receiver binary is not yet installable (aa_protocol_adapter is
# native-only; task-41 scope), and the task-29 udev policy was ROLLED BACK after
# its own QA round (reinstall as prerequisite 1 before a run). No live recovery
# claim is made until an invocation writes a PASS record.
#
# Fail-closed by construction: every ssh runs under `timeout`; a missing
# prerequisite writes a BLOCKED result and exits non-zero; exit 0 requires every
# selected scenario to OBSERVE its evidence (state/flow dip, real rejection,
# painted frames, a successful audio pump, "ok":"true" input) inside the wired
# 5 s budget. A green smoke without observed evidence is never a pass - a
# GetState=projecting string alone is NON-PASS by gate (P2-G).
#
# Scenarios:
#   usb_unplug_replug  sysfs unbind/bind the phone's USB device on the port
#                      (bounded and reversible; the phone stays functional)
#   receiver_restart   bounce the on-target receiver process
#   consumer_restart   bounce the dummy consumer (display) process
#   phone_lock_unlock  ADB keyevent lock + wake/unlock
#   phone_aa_restart   ADB force-stop Android Auto (phone-side restart)
#   unknown_phone      a second (S9 Edge) device must fail closed with an
#                      OBSERVED admission rejection; BLOCKED when the second
#                      device is absent or no rejection is observable
#   malformed_input    host-covered by the logic matrix; reported N/A with reason
#                      (no portable way to inject a malformed AAP frame from shell)
#
# Prerequisites (once per target):
#   1. task-29 udev policy installed: sudo -n bash tools/usb-policy/install.sh
#   2. approved phone on the qualified port (default 1-2.1) + trust store loaded
#   3. display connected + PipeWire sink present + aa-dummy-display available
#   4. an on-target aa-wired-receiver binary (see docs/qualification/wired-live-qa.md)
#   5. passwordless sudo for the sysfs unbind/bind step, adb authorized
#
# Usage:
#   run-wired-recovery-qa.sh --readiness-only [--out /tmp/task-31-wired-recovery.json]
#   run-wired-recovery-qa.sh --receiver /opt/aa/aa-wired-receiver \
#       --display /opt/aa/aa-dummy-display --store /home/zucchi/.local/state/... \
#       [--only usb_unplug_replug,consumer_restart] \
#       --out /tmp/task-31-wired-recovery.json
#
# Environment overrides: HOST (jetson.local), PORT (1-2.1), BUDGET_MS (5000),
# EVIDENCE_WAIT_MS (8000, per-scenario media/rejection evidence window; the
# gate-test seam, kept env-only so the CLI contract stays stable).
# A --only run is recorded as a SUBSET in the JSON (`only` + `subset` fields and
# the verdict text) - a subset PASS is never mistakable for a full-matrix PASS.

set -euo pipefail

# Host Python 3 is required. No remote or wall clock can authorize acceptance.
monotonic_ns() {
    local stamp
    stamp="$(python3 -c 'import time; print(time.monotonic_ns())')" || return 1
    [[ "$stamp" =~ ^(0|[1-9][0-9]{0,17})$ ]] || return 1
    printf '%s\n' "$stamp"
}
HARNESS_ENTRY_NS="$(monotonic_ns)" || { echo 'timing_error: host monotonic clock unavailable' >&2; exit 1; }

HOST="${HOST:-jetson.local}"
PORT="${PORT:-1-2.1}"
RECEIVER=""
DISPLAY=""
STORE=""
ADB_SERIAL=""
OUT="/tmp/task-31-wired-recovery.json"
BUDGET_MS="${BUDGET_MS:-5000}"
EVIDENCE_WAIT_MS="${EVIDENCE_WAIT_MS:-8000}"
READINESS_ONLY=0
ONLY=""
ONLY_SEEN=0
SCENARIOS=(usb_unplug_replug receiver_restart consumer_restart
          phone_lock_unlock phone_aa_restart unknown_phone malformed_input)
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
RESULTS_DIR="$(mktemp -d)"
RESULTS_TSV="${RESULTS_DIR}/results.tsv"
: > "${RESULTS_TSV}"

usage() {
    sed -n '2,55p' "$0"
    exit 2
}

while (($# > 0)); do
    case "$1" in
        --host) HOST="$2"; shift 2 ;;
        --port) PORT="$2"; shift 2 ;;
        --receiver) RECEIVER="$2"; shift 2 ;;
        --display) DISPLAY="$2"; shift 2 ;;
        --store) STORE="$2"; shift 2 ;;
        --adb-serial) ADB_SERIAL="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        --budget-ms) BUDGET_MS="$2"; shift 2 ;;
        --only)
            if (($# < 2)); then
                echo "refusing --only without a value; accepted values: ${SCENARIOS[*]}" >&2
                exit 2
            fi
            ONLY="$2"; ONLY_SEEN=1; shift 2 ;;
        --readiness-only) READINESS_ONLY=1; shift ;;
        -h|--help) usage ;;
        *) echo "unknown argument: $1" >&2; usage ;;
    esac
done

# Hostile-argument neutralization (gate P3): a leading-dash HOST would be read
# as an ssh OPTION (e.g. -oProxyCommand=...) and a quote-bearing PORT would
# break out of a remote command string. HOST is refused outright when it could
# be an option; every value that enters a remote command string goes through
# shq(); every value that enters the JSON goes through json_escape(); the host
# itself is never written raw (sha256:16 tag only).
if [[ "$HOST" == -* ]]; then
    echo "refusing hostile --host value (leading dash): must not be an ssh option" >&2
    exit 2
fi
if ! [[ "$BUDGET_MS" =~ ^[0-9]+$ ]]; then
    echo "refusing hostile --budget-ms value: must be a plain integer" >&2
    exit 2
fi
contract_budget_ms=5000
BUDGET_MS="$(python3 -c 'import sys; n=int(sys.argv[1]); print(n); sys.exit(not 1 <= n <= int(sys.argv[2]))' "$BUDGET_MS" "$contract_budget_ms")" || {
    echo 'refusing --budget-ms/BUDGET_MS: contract requires 1..5000' >&2
    exit 2
}
if ! [[ "$EVIDENCE_WAIT_MS" =~ ^[0-9]+$ ]]; then
    echo "refusing hostile EVIDENCE_WAIT_MS value: must be a plain integer" >&2
    exit 2
fi

# --only selection contract (gate N3): an empty value, an unknown name or a
# malformed list selects ZERO scenarios, and a verdict over zero recorded rows
# reads PASS - a vacuous pass over no evidence. Refuse at parse time with a
# usage error naming the accepted values: fail closed, never a PASS.
if (( ONLY_SEEN )); then
    only_ok=1
    if [[ -z "${ONLY}" ]]; then
        only_ok=0
    elif [[ ! "${ONLY}" =~ ^[a-z_]+(,[a-z_]+)*$ ]]; then
        only_ok=0
    else
        for token in ${ONLY//,/ }; do
            token_known=0
            for scenario in "${SCENARIOS[@]}"; do
                [[ "${token}" == "${scenario}" ]] && token_known=1
            done
            (( token_known )) || only_ok=0
        done
    fi
    if (( ! only_ok )); then
        echo "refusing --only value: expected a comma-separated list of scenario names; accepted values: ${SCENARIOS[*]}" >&2
        exit 2
    fi
fi

shq() { printf '%q' "$1"; }

# Bounded remote command (hung_commands class): 30 s hard ceiling per ssh.
# A batch ssh has no desktop session, so every remote command exports the
# session bus + display first: without them `busctl --user` fails with
# "Failed to get credentials: No such device or address" and the Qt display
# consumer aborts with no DISPLAY. $(id -u) must expand on the TARGET (escaped
# here); it is the ssh login user's uid, not this host's.
remote() {
    local limit=30 rc=0 now remaining
    if [[ -n "${STIMULUS_NS:-}" && "${CLEANING:-0}" == 0 && -z "${TERMINAL_NS:-}" ]]; then
        now="$(monotonic_ns)" || { printf timing_error > "$TIMING_FAULT"; return 125; }
        remaining=$(( DEADLINE_NS - now ))
        (( remaining > 0 )) || return 124
        printf -v limit '%d.%09d' "$(( remaining / 1000000000 ))" "$(( remaining % 1000000000 ))"
    fi
    timeout "$limit" ssh "${SSH_OPTS[@]}" -- "$HOST" \
        "export DBUS_SESSION_BUS_ADDRESS=\"unix:path=/run/user/\$(id -u)/bus\"; export DISPLAY=:1; $*" || rc=$?
    if [[ -n "${STIMULUS_NS:-}" && "${CLEANING:-0}" == 0 && -z "${TERMINAL_NS:-}" ]]; then
        now="$(monotonic_ns)" || { printf timing_error > "$TIMING_FAULT"; return 125; }
        (( now < DEADLINE_NS )) || return 124
    fi
    return "$rc"
}

json_escape() { printf '%s' "$1" | tr -d '\000' | sed 's/\\/\\\\/g; s/"/\\"/g'; }

host_tag="$(printf '%s' "$HOST" | sha256sum | cut -c1-16)"
PORT_Q="$(shq "$PORT")"

# Append one scenario record: name, status, recovery_ms (or ""), retries (or ""),
# evidence, detail, frames_painted_baseline (or ""). '|' (not tab) is the field
# separator: tab is IFS whitespace and collapses the empty recovery/retries
# fields, which shifted values into the wrong JSON slots.
FRAMES_BASELINE=""
SCENARIO_NAME=""
STIMULUS_NS=""
DEADLINE_NS=""
TERMINAL_NS=""
TERMINAL_SCENARIO=""
TIMING_FAULT="${RESULTS_DIR}/timing-fault"
RECORDED=0
CLEANING=0

reset_scenario() {
    SCENARIO_NAME="$1"
    STIMULUS_NS="" DEADLINE_NS="" TERMINAL_NS="" TERMINAL_SCENARIO=""
    FRAMES_BASELINE="" RECORDED=0
    : > "$TIMING_FAULT"
}

arm_scenario() {
    if [[ -n "$STIMULUS_NS" ]]; then
        printf timing_error_rearmed > "$TIMING_FAULT"
        return 1
    fi
    STIMULUS_NS="$(monotonic_ns)" || { printf timing_error_clock > "$TIMING_FAULT"; return 1; }
    DEADLINE_NS=$(( STIMULUS_NS + BUDGET_MS * 1000000 ))
}

# Called in the parent only AFTER all scenario-specific terminal predicates.
complete_scenario() {
    TERMINAL_SCENARIO="$SCENARIO_NAME"
    TERMINAL_NS="$(monotonic_ns)" || { printf timing_error_clock > "$TIMING_FAULT"; return 1; }
}

run_scenario() {
    local combined_results="$RESULTS_TSV"
    RESULTS_TSV="${RESULTS_DIR}/scenario.tsv"
    : > "$RESULTS_TSV"
    reset_scenario "$1"
    "scenario_$1"
    if (( RECORDED != 1 )); then
        : > "$RESULTS_TSV"
        RECORDED=0
        record "$1" FAILED "" "" "" scenario_result_count_invalid
    fi
    cat "$RESULTS_TSV" >> "$combined_results"
    RESULTS_TSV="$combined_results"
}

record() {
    local name="$1" status="$2" recovery="" retries="$4" evidence="$5" detail="$6"
    local now elapsed="" delta
    if [[ -n "$STIMULUS_NS" || "$status" == PASS || -s "$TIMING_FAULT" ]]; then
        now="$(monotonic_ns)" || now=""
        if [[ -s "$TIMING_FAULT" || ! "$STIMULUS_NS" =~ ^(0|[1-9][0-9]{0,17})$ ||
              ! "$DEADLINE_NS" =~ ^(0|[1-9][0-9]{0,17})$ ||
              ! "$now" =~ ^(0|[1-9][0-9]{0,17})$ || "$name" != "$SCENARIO_NAME" ]] ||
              (( now < STIMULUS_NS || DEADLINE_NS != STIMULUS_NS + BUDGET_MS * 1000000 )); then
            status=FAILED detail=timing_error_invalid_context
        else
            elapsed="$(( (now - STIMULUS_NS) / 1000000 ))"
            if [[ -n "$TERMINAL_NS" ]]; then
                if [[ ! "$TERMINAL_NS" =~ ^(0|[1-9][0-9]{0,17})$ || "$TERMINAL_SCENARIO" != "$name" ]] ||
                    (( TERMINAL_NS < STIMULUS_NS || TERMINAL_NS > now )); then
                    status=FAILED detail=timing_error_invalid_terminal
                else
                    delta=$(( TERMINAL_NS - STIMULUS_NS ))
                    # Compare ns BEFORE converting to the reporting unit.
                    if (( delta >= BUDGET_MS * 1000000 )); then
                        status=FAILED detail=recovery_exceeded_budget
                    fi
                    recovery="$(printf '%d.%06d' "$(( delta / 1000000 ))" "$(( delta % 1000000 ))")"
                fi
            elif [[ "$status" == PASS ]]; then
                status=FAILED detail=timing_error_missing_terminal
            elif (( now >= DEADLINE_NS )); then
                status=FAILED detail=recovery_exceeded_budget
            fi
        fi
    fi
    printf '%s|%s|%s|%s|%s|%s|%s|%s\n' "$name" "$status" "$recovery" "$retries" "$evidence" "$detail" "${FRAMES_BASELINE}" "$elapsed" >> "${RESULTS_TSV}"
    RECORDED=$(( RECORDED + 1 ))
    FRAMES_BASELINE=""
}

probe_deadline() {
    local now
    now="$(monotonic_ns)" || { printf timing_error_clock > "$TIMING_FAULT"; return 1; }
    PROBE_DEADLINE_NS=$(( now + $1 * 1000000 ))
    if [[ -n "$STIMULUS_NS" ]] && (( DEADLINE_NS < PROBE_DEADLINE_NS )); then
        PROBE_DEADLINE_NS="$DEADLINE_NS"
    fi
}

before_deadline() {
    local now
    now="$(monotonic_ns)" || { printf timing_error_clock > "$TIMING_FAULT"; return 1; }
    (( now < $1 ))
}

# D-Bus projection state (the same contract the task-30 harness uses).
projecting() {
    local state
    state="$(remote "busctl --user call org.custom.AndroidAutoReceiver /org/custom/AndroidAutoReceiver org.custom.AndroidAutoReceiver1 GetState 2>/dev/null" || true)"
    [[ "${state}" == *projecting* ]]
}

wait_projecting() {
    probe_deadline "$1" || return 1
    local deadline="$PROBE_DEADLINE_NS"
    while before_deadline "$deadline"; do
        if projecting; then
            before_deadline "$deadline"; return $?
        fi
        sleep 0.2
    done
    return 1
}

wait_not_projecting() {
    probe_deadline "$1" || return 1
    local deadline="$PROBE_DEADLINE_NS"
    while before_deadline "$deadline"; do
        if ! projecting; then
            before_deadline "$deadline"; return $?
        fi
        sleep 0.2
    done
    return 1
}

# Current receiver-log size (byte offset marker for per-scenario windows).
log_offset() {
    remote "wc -c < /tmp/aa-wired-receiver.log 2>/dev/null || echo 0" | tr -dc '0-9' || echo 0
}

# Receiver-log content written after the given byte offset.
log_since() {
    remote "tail -c +$(( $1 + 1 )) /tmp/aa-wired-receiver.log 2>/dev/null" || true
}

# The receiver's run-cumulative reconnect-attempt counter ("retries":N).
retries_total() {
    remote "grep -o '\"retries\":[0-9]*' /tmp/aa-wired-receiver.log 2>/dev/null | grep -o '[0-9]*' | sort -n | tail -1" || true
}

# Per-disturbance retry delta (item 7): the attempts attributed to THIS
# disturbance (counter after minus counter at the disturbance), never the
# run-cumulative maximum. A counter that reset with a restarted receiver reads
# as 0 for the pre-disturbance baseline and the delta counts the fresh log.
retry_delta() {
    local before="${1:-0}" after delta
    after="$(retries_total)"
    [[ "${before}" =~ ^[0-9]+$ ]] || before=0
    [[ "${after}" =~ ^[0-9]+$ ]] || after=0
    delta=$(( after - before ))
    if (( delta < 0 )); then
        delta=0
    fi
    printf '%s\n' "${delta}"
}

# ---- observed-evidence probes (gate P2: PASS requires OBSERVED evidence) ----

# Painted-frames evidence from the SHIPPED dummy display's test-only status
# channel (task 24 testcontrol). Item 8: the baseline counter is recorded at
# the recovery start and post-recovery frames must GROW past it with render_ok
# - a stuck counter is not post-recovery painting (video_sent alone only proves
# units crossed the channel, not that anything painted).
painted_frame_probe() {
    local line
    line="$(remote "printf 'status\n' > /tmp/aa-disp.in 2>/dev/null && sleep 0.3 && grep '\"event\":\"status\"' /tmp/aa-wired-display.log 2>/dev/null | tail -1" 2>/dev/null)" || return 1
    # Parse the whole JSON value, never an integer prefix; compare in Python so
    # unsigned display counters cannot overflow Bash's signed arithmetic.
    python3 -c '
import json, sys
try:
    status = json.loads(sys.argv[1])
except (ValueError, RecursionError):
    sys.exit(1)
if not isinstance(status, dict):
    sys.exit(1)
count = status.get("frames_painted")
if type(count) is not int or not 0 <= count <= 2**64 - 1:
    sys.exit(1)
if sys.argv[2]:
    baseline = int(sys.argv[2])
    if not 0 <= baseline < count or count <= 0 or status.get("render_ok") is not True:
        sys.exit(1)
print(count)
' "$line" "${1:-}"
}

frames_painted_baseline() {
    painted_frame_probe
}

painted_frames_grew() {
    local baseline="${1:-}"
    [[ "$baseline" =~ ^(0|[1-9][0-9]{0,19})$ ]] || return 1
    painted_frame_probe "$baseline" >/dev/null
}

# Successful audio pump evidence: audio_chunks is logged BEFORE router_.pump()
# runs (so its presence proves nothing about the sink); emit_media_timing is
# emitted ONLY after pump() succeeds. media_timing is the pump-success marker.
audio_pumped() {
    local since="${1:-0}"
    remote "tail -c +$(( since + 1 )) /tmp/aa-wired-receiver.log 2>/dev/null | grep -q '\"event\":\"media_timing\"' && echo ok" >/dev/null 2>&1
}

# Input delivery evidence: input_forwarded presence alone is not a delivery
# claim - only the ok:true record is ("ok":"false" means the send failed).
input_ok() {
    local since="${1:-0}"
    remote "tail -c +$(( since + 1 )) /tmp/aa-wired-receiver.log 2>/dev/null | grep -q '\"event\":\"input_forwarded\",\"ok\":\"true\"' && echo ok" >/dev/null 2>&1
}

# Drive one real display key event through the shipped testcontrol seam so the
# input path produces an observable input_forwarded record.
stimulate_input() {
    remote "printf 'key a press\nkey a release\n' > /tmp/aa-disp.in 2>/dev/null" >/dev/null 2>&1 || true
}

# Observed disturbance in the scenario's log window: the session's own
# reconnect telemetry (link lost / peer disconnected). Absence means the
# stimulus never reached the session - BLOCKED, never PASS (gate P2: a recovery
# claim without an observed dip is vacuous).
dip_observed() {
    local since="${1:-0}"
    local window
    window="$(log_since "${since}")"
    [[ "$window" == *'"kind":"reconnect"'* ]] && return 0
    [[ "$window" == *'peer disconnected'* ]] && return 0
    return 1
}

# Observed rejection of an untrusted device: admission/link failure telemetry
# (transport_unauthorized_peer / session_unknown_phone 302 / connect failure).
# The session diag emits 302 as a NUMERIC JSON value ("code":302, unquoted) -
# the quoted "code":"302" shape never appears in the real log (N2).
rejection_shape() {
    local window="${1:-}"
    [[ "$window" == *transport_unauthorized_peer* ]] && return 0
    [[ "$window" == *session_unknown_phone* ]] && return 0
    [[ "$window" == *'"code":302'* ]] && return 0
    [[ "$window" == *connect-failed* ]] && return 0
    return 1
}

# The ATTRIBUTED rejection (item 2): the receiver's link-error record at
# admission rejection carries ADDITIVE identity+port fields for the rejected
# attempt (tools/wired-receiver/main.cpp - "phone_id" + "port"; existing fields
# unchanged). Prints "phone_id<TAB>port" for the first record that names the
# link error AND a rejection code AND both attribution fields; fails when the
# rejection cannot be attributed to a concrete attempt.
attributed_rejection() {
    local window="${1:-}" line phone_id port
    local id_re='"phone_id":"([^"]*)"'
    local port_re='"port":"([^"]*)"'
    while IFS= read -r line; do
        [[ "$line" == *'"event":"err"'* ]] || continue
        [[ "$line" == *'"detail":"link"'* ]] || continue
        rejection_shape "$line" || continue
        [[ "$line" =~ $id_re ]] || continue
        phone_id="${BASH_REMATCH[1]}"
        [[ "$line" =~ $port_re ]] || continue
        port="${BASH_REMATCH[1]}"
        [[ -n "$phone_id" && -n "$port" ]] || continue
        printf '%s\t%s\n' "$phone_id" "$port"
        return 0
    done <<< "$window"
    return 1
}

# Full media evidence required before any recovery PASS: painted frames that
# GREW past the recorded baseline AND a successful audio pump AND a delivered
# input event (stimulated here).
media_evidence() {
    local since="${1:-0}" frames_baseline="${2:-}"
    stimulate_input
    probe_deadline "$EVIDENCE_WAIT_MS" || return 1
    local deadline="$PROBE_DEADLINE_NS"
    while before_deadline "$deadline"; do
        if painted_frames_grew "${frames_baseline}" && audio_pumped "${since}" && input_ok "${since}"; then
            before_deadline "$deadline"; return $?
        fi
        sleep 0.5
    done
    return 1
}

RECEIVER_PID=""
CONSUMER_PID=""
start_receiver() {
    [[ -n "${RECEIVER}" ]] || return 1
    local args="--port ${PORT_Q}"
    [[ -n "${STORE}" ]] && args="${args} --store $(shq "$STORE")"
    RECEIVER_PID="$(remote "nohup $(shq "$RECEIVER") ${args} > /tmp/aa-wired-receiver.log 2>&1 & echo \$!" 2>/dev/null | tr -dc '0-9')"
    [[ -n "${RECEIVER_PID}" ]]
}

start_consumer() {
    [[ -n "${DISPLAY}" ]] || return 1
    # The display runs with the shipped test-only stdin control channel open on
    # a FIFO (<> keeps the open non-blocking): it is the painted-frames and
    # input-stimulation evidence source for the PASS gates.
    CONSUMER_PID="$(remote "mkfifo /tmp/aa-disp.in 2>/dev/null || true; nohup $(shq "$DISPLAY") --test-only-autostart --test-only-control-stdin <>/tmp/aa-disp.in > /tmp/aa-wired-display.log 2>&1 & echo \$!" 2>/dev/null | tr -dc '0-9')"
    [[ -n "${CONSUMER_PID}" ]]
}

# Process liveness BY PID (never `pgrep -f`: the remote command line contains
# the pattern, so pgrep -f self-matches and always reports alive).
proc_alive() {
    [[ -n "${1:-}" ]] || return 1
    remote "kill -0 $1 2>/dev/null && echo alive || echo gone" 2>/dev/null | grep -q alive
}

# Explicit per-PID state: only a literal "gone" observes termination - a probe
# that failed or reported anything else is NOT death evidence (fail closed).
proc_state() {
    remote "kill -0 $1 2>/dev/null && echo alive || echo gone" 2>/dev/null | tr -dc 'a-z'
}

scenario_selected() {
    [[ -z "${ONLY}" || ",${ONLY}," == *",$1,"* ]]
}

# adb `devices -l` maps each device to its USB port through the real
# `usb:<port>` device-info field: prints "serial<TAB>port" per device.
adb_port_map() {
    local listing line serial port tok
    listing="$(remote "adb devices -l 2>/dev/null" 2>/dev/null || true)"
    while IFS= read -r line; do
        serial="${line%%[[:space:]]*}"
        [[ -z "${serial}" || "${serial}" == List* ]] && continue
        port=""
        for tok in ${line}; do
            [[ "${tok}" == usb:* ]] && port="${tok#usb:}"
        done
        [[ -n "${port}" ]] || continue
        printf '%s\t%s\n' "${serial}" "${port}"
    done <<< "${listing}"
}

# One bounded admission attempt against the device on `port`, with the same
# store: an unapproved device fails admission and the receiver records the
# link-error event (exit 3). Distinct sockets and --no-bus keep the attempt
# from contending with the live session's endpoints.
ATTEMPT_RC=0
drive_admission_attempt() {
    local args="--port $(shq "$1")"
    [[ -n "${STORE}" ]] && args="${args} --store $(shq "$STORE")"
    ATTEMPT_RC=0
    remote "$(shq "$RECEIVER") ${args} --no-bus --duration-ms 3000 --video-socket /tmp/aa-attempt-video.sock --input-socket /tmp/aa-attempt-input.sock > /tmp/aa-wired-receiver-attempt.log 2>&1" >/dev/null 2>&1 || ATTEMPT_RC=$?
}

attempt_window() {
    remote "tail -c +1 /tmp/aa-wired-receiver-attempt.log 2>/dev/null" || true
}

# The intended stimulus device: the approved phone selected through the same
# adb port-match attribution (usb:<PORT>, or the ADB_SERIAL override). Prints
# its adb serial; fails when no device maps to the approved phone.
stimulus_serial() {
    local map serial port
    map="$(adb_port_map || true)"
    while IFS=$'\t' read -r serial port; do
        [[ -n "${serial}" ]] || continue
        if [[ "${port}" == "${PORT}" ]] ||
            { [[ -n "${ADB_SERIAL}" ]] && [[ "${serial}" == "${ADB_SERIAL}" ]]; }; then
            printf '%s\n' "${serial}"
            return 0
        fi
    done <<< "${map}"
    [[ -n "${ADB_SERIAL}" ]] || return 1
    printf '%s\n' "${ADB_SERIAL}"
}

# One stimulus on the INTENDED device (adb -s <serial>, never bare adb): the
# return code is the claim - a failed stimulus is a FAILED scenario, never
# swallowed (item 3 fail-closed).
adb_stimulus() {
    local serial="$1"
    shift
    remote "adb -s $(shq "$serial") $*"
}

# The intended device's attribution label: the first phone_id the receiver
# recorded (the admitted phone's pseudonym; raw keys never cross the boundary).
intended_phone_label() {
    local window line
    window="$(log_since 0)"
    local id_re='"phone_id":"([^"]*)"'
    while IFS= read -r line; do
        [[ "$line" =~ $id_re ]] || continue
        printf '%s\n' "${BASH_REMATCH[1]}"
        return 0
    done <<< "$window"
    return 1
}

# The observed state/flow dip must correlate to the intended device via the
# same identity attribution (item 3): the dip record carries phone_id and it
# must name the intended (admitted) phone - a transition attributed to any
# other identity is not this device's recovery.
dip_attributed() {
    local since="${1:-0}" label="${2:-}"
    [[ -n "${label}" ]] || return 1
    local window line
    window="$(log_since "${since}")"
    while IFS= read -r line; do
        [[ "$line" == *'"kind":"reconnect"'* || "$line" == *'peer disconnected'* ]] || continue
        [[ "$line" == *"\"phone_id\":\"${label}\""* ]] && return 0
    done <<< "$window"
    return 1
}

# ---- scenario implementations (each records exactly one row) ----
# PASS is earned only with observed evidence: stimulus outcome + disturbance
# (dip/rejection) + recovery inside the budget window (bounded-wait evidence)
# + real media delivery. Anything unobservable is BLOCKED, never PASS.

scenario_receiver_restart() {
    local recovery="" offset retries_before
    # The stack is already up and projecting (item 4 setup): this scenario only
    # disturbs it and restarts the receiver.
    if ! wait_projecting "${BUDGET_MS}"; then
        record receiver_restart FAILED "" "" "" "no_initial_projection"
        return
    fi
    # Kill and verify the tracked child BY PID (item 5): pattern kills
    # (pkill/pgrep -f) self-match on the remote command line and can neither
    # kill nor observe this specific child's death.
    FRAMES_BASELINE="$(frames_painted_baseline)" ||
        { record receiver_restart FAILED "" "" "" frames_baseline_unavailable; return; }
    retries_before=0
    arm_scenario || { record receiver_restart FAILED "" "" "" timing_error_clock; return; }
    remote "kill ${RECEIVER_PID} 2>/dev/null || true" >/dev/null 2>&1 ||
        { record receiver_restart FAILED "" "" "" stimulus_failed; return; }
    sleep 1
    # The receiver's process death is the observed state dip (gate P2).
    if [[ "$(proc_state "${RECEIVER_PID}")" != "gone" ]]; then
        record receiver_restart FAILED "" "" "dip_unobserved" "receiver_did_not_stop"
        return
    fi
    wait_not_projecting 3000 || true
    start_receiver || { record receiver_restart FAILED "" "" "" "restart_failed"; return; }
    # start_receiver launches with `> /tmp/aa-wired-receiver.log` (truncate):
    # the pre-restart offset points past EOF in the fresh log and would miss
    # genuine post-recovery media. The evidence window is the fresh log from
    # byte 0 (N4) and the retry baseline restarts with it (item 7).
    offset=0
    if wait_projecting "${BUDGET_MS}"; then
        if ! remote "grep -q '\"event\":\"active\"' /tmp/aa-wired-receiver.log 2>/dev/null && echo ok" >/dev/null 2>&1; then
            record receiver_restart FAILED "${recovery}" "$(retry_delta "${retries_before}")" "session_active_not_observed" "no_active_event"
            return
        fi
        if media_evidence "${offset}" "${FRAMES_BASELINE}"; then
            complete_scenario || true
            record receiver_restart PASS "" "$(retry_delta "${retries_before}")" "process_dip+active+frames+pump+input" "recovered"
        else
            record receiver_restart FAILED "${recovery}" "$(retry_delta "${retries_before}")" "process_dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record receiver_restart FAILED "" "$(retry_delta "${retries_before}")" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_consumer_restart() {
    local recovery="" offset retries_before
    # The stack is already up and projecting (item 4 setup): this scenario only
    # disturbs it and restarts the display.
    wait_projecting "${BUDGET_MS}" || { record consumer_restart FAILED "" "" "" "no_initial_projection"; return; }
    # Kill and verify the tracked child BY PID (item 5), never by pattern.
    offset="$(log_offset)"
    retries_before="$(retries_total)"
    FRAMES_BASELINE="$(frames_painted_baseline)" ||
        { record consumer_restart FAILED "" "" "" frames_baseline_unavailable; return; }
    arm_scenario || { record consumer_restart FAILED "" "" "" timing_error_clock; return; }
    remote "kill ${CONSUMER_PID} 2>/dev/null || true" >/dev/null 2>&1 ||
        { record consumer_restart FAILED "" "" "" stimulus_failed; return; }
    sleep 1
    # The consumer's process death is the observed state dip (gate P2).
    if [[ "$(proc_state "${CONSUMER_PID}")" != "gone" ]]; then
        record consumer_restart FAILED "" "" "dip_unobserved" "display_did_not_stop"
        return
    fi
    start_consumer || { record consumer_restart FAILED "" "" "" "restart_failed"; return; }
    # The restarted display owns a new counter: require an observed baseline
    # again, not a fabricated zero or the previous process's counter.
    FRAMES_BASELINE="$(frames_painted_baseline)" ||
        { record consumer_restart FAILED "" "" "" frames_baseline_unavailable; return; }
    if wait_projecting "${BUDGET_MS}"; then
        if media_evidence "${offset}" "${FRAMES_BASELINE}"; then
            complete_scenario || true
            record consumer_restart PASS "" "$(retry_delta "${retries_before}")" "process_dip+frames+pump+input" "recovered"
        else
            record consumer_restart FAILED "${recovery}" "$(retry_delta "${retries_before}")" "process_dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record consumer_restart FAILED "" "$(retry_delta "${retries_before}")" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_usb_unplug_replug() {
    local recovery="" offset retries_before
    wait_projecting "${BUDGET_MS}" || { record usb_unplug_replug FAILED "" "" "" "no_initial_projection"; return; }
    offset="$(log_offset)"
    retries_before="$(retries_total)"
    FRAMES_BASELINE="$(frames_painted_baseline)" ||
        { record usb_unplug_replug FAILED "" "" "" frames_baseline_unavailable; return; }
    arm_scenario || { record usb_unplug_replug FAILED "" "" "" timing_error_clock; return; }
    UNBOUND_PORT="${PORT}"
    if ! remote "sudo -n sh -c 'echo -n ${PORT_Q} > /sys/bus/usb/drivers/usb/unbind'" >/dev/null 2>&1; then
        record usb_unplug_replug FAILED "" "" "" "sysfs_unbind_requires_passwordless_sudo"
        return
    fi
    sleep 1
    if ! remote "sudo -n sh -c 'echo -n ${PORT_Q} > /sys/bus/usb/drivers/usb/bind'" >/dev/null 2>&1; then
        record usb_unplug_replug FAILED "" "" "" "sysfs_bind_requires_passwordless_sudo"
        return
    fi
    UNBOUND_PORT=""
    if wait_projecting "${BUDGET_MS}"; then
        if ! dip_observed "${offset}"; then
            record usb_unplug_replug BLOCKED "${recovery}" "$(retry_delta "${retries_before}")" "dip_unobserved" "no_state_or_flow_dip_observed"
            return
        fi
        if media_evidence "${offset}" "${FRAMES_BASELINE}"; then
            complete_scenario || true
            record usb_unplug_replug PASS "" "$(retry_delta "${retries_before}")" "dip+frames+pump+input" "recovered_on_replug"
        else
            record usb_unplug_replug FAILED "${recovery}" "$(retry_delta "${retries_before}")" "dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record usb_unplug_replug FAILED "" "$(retry_delta "${retries_before}")" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_phone_lock_unlock() {
    local recovery="" offset retries_before serial label
    wait_projecting "${BUDGET_MS}" || { record phone_lock_unlock FAILED "" "" "" "no_initial_projection"; return; }
    serial="$(stimulus_serial)" || { record phone_lock_unlock BLOCKED "" "" "" "stimulus_device_unattributed"; return; }
    offset="$(log_offset)"
    retries_before="$(retries_total)"
    FRAMES_BASELINE="$(frames_painted_baseline)" ||
        { record phone_lock_unlock FAILED "" "" "" frames_baseline_unavailable; return; }
    label="$(intended_phone_label)" || label=""
    arm_scenario || { record phone_lock_unlock FAILED "" "" "" timing_error_clock; return; }
    adb_stimulus "${serial}" "shell input keyevent 26" >/dev/null 2>&1 ||
        { record phone_lock_unlock FAILED "" "" "" "stimulus_failed"; return; }
    sleep 2
    adb_stimulus "${serial}" "shell input keyevent 224" >/dev/null 2>&1 ||
        { record phone_lock_unlock FAILED "" "" "" "stimulus_failed"; return; }
    adb_stimulus "${serial}" "shell input swipe 540 1600 540 400" >/dev/null 2>&1 ||
        { record phone_lock_unlock FAILED "" "" "" "stimulus_failed"; return; }
    if wait_projecting "${BUDGET_MS}"; then
        if ! dip_observed "${offset}"; then
            record phone_lock_unlock BLOCKED "${recovery}" "$(retry_delta "${retries_before}")" "dip_unobserved" "no_state_or_flow_dip_observed"
            return
        fi
        if ! dip_attributed "${offset}" "${label}"; then
            record phone_lock_unlock FAILED "${recovery}" "$(retry_delta "${retries_before}")" "dip_unattributed" "dip_not_attributed_to_intended_device"
            return
        fi
        if media_evidence "${offset}" "${FRAMES_BASELINE}"; then
            complete_scenario || true
            record phone_lock_unlock PASS "" "$(retry_delta "${retries_before}")" "dip+frames+pump+input" "recovered_after_unlock"
        else
            record phone_lock_unlock FAILED "${recovery}" "$(retry_delta "${retries_before}")" "dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record phone_lock_unlock FAILED "" "$(retry_delta "${retries_before}")" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_phone_aa_restart() {
    local recovery="" offset retries_before serial label
    wait_projecting "${BUDGET_MS}" || { record phone_aa_restart FAILED "" "" "" "no_initial_projection"; return; }
    serial="$(stimulus_serial)" || { record phone_aa_restart BLOCKED "" "" "" "stimulus_device_unattributed"; return; }
    offset="$(log_offset)"
    retries_before="$(retries_total)"
    FRAMES_BASELINE="$(frames_painted_baseline)" ||
        { record phone_aa_restart FAILED "" "" "" frames_baseline_unavailable; return; }
    label="$(intended_phone_label)" || label=""
    arm_scenario || { record phone_aa_restart FAILED "" "" "" timing_error_clock; return; }
    adb_stimulus "${serial}" "shell am force-stop com.google.android.projection.gearhead" >/dev/null 2>&1 ||
        { record phone_aa_restart FAILED "" "" "" "stimulus_failed"; return; }
    if wait_projecting "${BUDGET_MS}"; then
        if ! dip_observed "${offset}"; then
            record phone_aa_restart BLOCKED "${recovery}" "$(retry_delta "${retries_before}")" "dip_unobserved" "no_state_or_flow_dip_observed"
            return
        fi
        if ! dip_attributed "${offset}" "${label}"; then
            record phone_aa_restart FAILED "${recovery}" "$(retry_delta "${retries_before}")" "dip_unattributed" "dip_not_attributed_to_intended_device"
            return
        fi
        if media_evidence "${offset}" "${FRAMES_BASELINE}"; then
            complete_scenario || true
            record phone_aa_restart PASS "" "$(retry_delta "${retries_before}")" "dip+frames+pump+input" "recovered_after_aa_restart"
        else
            record phone_aa_restart FAILED "${recovery}" "$(retry_delta "${retries_before}")" "dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record phone_aa_restart FAILED "" "$(retry_delta "${retries_before}")" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_unknown_phone() {
    # A second, unapproved device must drive its OWN admission attempt and the
    # rejection must be OBSERVED and ATTRIBUTED to that attempt - a quiet log,
    # or a rejection that cannot be attributed, never proves fail-closed.
    # (a) map ADB devices to USB ports via `adb devices -l` (usb:<port>): the
    # second device is selected by port match and must differ from the approved
    # phone in BOTH identity (serial) and port.
    local map approved_serial="" d2_serial="" d2_port="" serial port
    map="$(adb_port_map || true)"
    while IFS=$'\t' read -r serial port; do
        [[ -n "${serial}" ]] || continue
        if [[ "${port}" == "${PORT}" ]] ||
            { [[ -n "${ADB_SERIAL}" ]] && [[ "${serial}" == "${ADB_SERIAL}" ]]; }; then
            approved_serial="${serial}"
        fi
    done <<< "${map}"
    while IFS=$'\t' read -r serial port; do
        [[ -n "${serial}" ]] || continue
        [[ "${serial}" == "${approved_serial}" ]] && continue
        [[ "${port}" == "${PORT}" ]] && continue
        d2_serial="${serial}"
        d2_port="${port}"
        break
    done <<< "${map}"
    if [[ -z "${d2_serial}" ]]; then
        record unknown_phone BLOCKED "" "" "" "second_device_absent_phase_a_fixture_covers_logic"
        return
    fi
    # (b) drive the SECOND device through admission: one bounded receiver run
    # on ITS port with the same store. The link-error record of that attempt
    # carries the rejected attempt's identity+port (additive fields).
    arm_scenario || { record unknown_phone FAILED "" "" "" timing_error_clock; return; }
    drive_admission_attempt "${d2_port}"
    local window attribution phone_id attempt_port
    window="$(attempt_window)"
    case "${ATTEMPT_RC}" in
        3) ;;
        0)
            record unknown_phone FAILED "" "" "admission_accepted" "unknown_device_admitted"
            return ;;
        *)
            record unknown_phone BLOCKED "" "" "" "admission_attempt_not_observable"
            return ;;
    esac
    # (c) assert the attributed rejection: the identity/port name the second
    # device's OWN attempt and differ from the approved phone's.
    if attribution="$(attributed_rejection "${window}")"; then
        phone_id="${attribution%%$'\t'*}"
        attempt_port="${attribution#*$'\t'}"
        if [[ "${attempt_port}" == "${PORT}" ]]; then
            record unknown_phone FAILED "" "" "rejection+approved_port" "rejection_attributed_to_approved_phone"
            return
        fi
        if [[ "${attempt_port}" != "${d2_port}" ]]; then
            record unknown_phone FAILED "" "" "rejection+other_port" "rejection_attributed_to_other_device"
            return
        fi
        if [[ "${window}" == *'"event":"active"'* || "${window}" == *'"event":"ready"'* ]]; then
            record unknown_phone FAILED "" "" "rejection+activation" "unknown_device_reached_active"
            return
        fi
        complete_scenario || true
        record unknown_phone PASS "" "" "attributed_rejection" "unknown_device_rejected_fail_closed"
        return
    fi
    if rejection_shape "${window}"; then
        record unknown_phone BLOCKED "" "" "rejection_unattributed" "rejection_unattributed_cannot_claim_fail_closed"
        return
    fi
    record unknown_phone BLOCKED "" "" "rejection_unobserved" "no_rejection_observed_cannot_claim_fail_closed"
}

scenario_malformed_input() {
    record malformed_input N/A "" "" "" "host_covered_by_ctest_wired_recovery_no_shell_injector"
}

# The phone's USB port while it is unbound (item 6): the cleanup must rebind a
# port left unbound by a failed/aborted scenario - never leave the device
# unbound after the run.
UNBOUND_PORT=""
rebind_unbound() {
    if [[ -n "${UNBOUND_PORT}" ]]; then
        remote "sudo -n sh -c 'echo -n $(shq "${UNBOUND_PORT}") > /sys/bus/usb/drivers/usb/bind'" >/dev/null 2>&1 || true
        UNBOUND_PORT=""
    fi
}

cleanup_target() {
    CLEANING=1
    rebind_unbound
    # Teardown by tracked PID (item 5): pattern kills (pkill -f) self-match the
    # remote wrapper shell and can kill the wrong process.
    [[ -n "${CONSUMER_PID}" ]] && remote "kill ${CONSUMER_PID} 2>/dev/null || true" >/dev/null 2>&1 || true
    [[ -n "${RECEIVER_PID}" ]] && remote "kill ${RECEIVER_PID} 2>/dev/null || true" >/dev/null 2>&1 || true
}

write_result() {
    local overall_status="$1" blocked_on="$2"
    cleanup_target
    trap - EXIT
    local wall_end wall_ms=null
    wall_end="$(monotonic_ns)" || wall_end=""
    if [[ -n "$wall_end" ]]; then wall_ms=$(( (wall_end - HARNESS_ENTRY_NS) / 1000000 )); fi
    local subset="false"
    local only_field="null"
    local verdict_note="phase-A host-side acceptance is separate (ctest -R wired_recovery); this is the live record"
    if [[ -n "${ONLY}" ]]; then
        subset="true"
        only_field="\"$(json_escape "${ONLY}")\""
        verdict_note="SUBSET RUN (only=${ONLY}) - NOT a full-matrix pass; only the selected scenarios were exercised. Phase-A host-side acceptance is separate (ctest -R wired_recovery)"
    fi
    mkdir -p "$(dirname "$OUT")"
    {
        printf '{\n'
        printf '  "task": "31. Implement wired reconnect and recovery matrix",\n'
        printf '  "phase": "B (live device)",\n'
        printf '  "host": "sha256:%s",\n' "${host_tag}"
        printf '  "port": "%s",\n' "$(json_escape "$PORT")"
        printf '  "budget_ms": %s,\n' "$BUDGET_MS"
        printf '  "wall_ms": %s,\n' "$wall_ms"
        printf '  "only": %s,\n' "${only_field}"
        printf '  "subset": %s,\n' "${subset}"
        printf '  "status": "%s",\n' "$overall_status"
        printf '  "blocked_on": "%s",\n' "$(json_escape "$blocked_on")"
        printf '  "scenarios": [\n'
        local first=1 name status recovery retries evidence detail frames_baseline elapsed
        while IFS='|' read -r name status recovery retries evidence detail frames_baseline elapsed; do
            [[ -n "${name}" ]] || continue
            (( first )) || printf ',\n'
            first=0
            printf '    {"name": "%s", "status": "%s", "recovery_ms": %s, "retries": %s, "evidence": "%s", "detail": "%s", "frames_painted_baseline": %s, "elapsed_ms": %s}' \
                "$(json_escape "$name")" "$(json_escape "$status")" \
                "${recovery:-null}" "${retries:-null}" \
                "$(json_escape "$evidence")" "$(json_escape "$detail")" \
                "${frames_baseline:-null}" "${elapsed:-null}"
        done < "${RESULTS_TSV}"
        printf '\n  ],\n'
        printf '  "budget_note": "recovery_ms is host-monotonic first-stimulus to COMPLETE terminal observation, ENFORCED strictly < %s ms on every PASS; exactly the boundary is FAILED; elapsed_ms includes partial observation and wall_ms includes setup and completed cleanup",\n' "$BUDGET_MS"
        printf '  "note": "%s"\n' "$(json_escape "${verdict_note}")"
        printf '}\n'
    } > "$OUT"
    echo "wrote $OUT (status=$overall_status${blocked_on:+ blocked_on=$blocked_on})"
}

if [[ "${BASH_SOURCE[0]}" != "$0" ]]; then return 0; fi

# ---- 1. reachability (bounded) ----
if ! remote "true" >/dev/null 2>&1; then
    record ssh BLOCKED "" "" "" "ssh_unreachable"
    write_result "BLOCKED" "ssh_unreachable"
    echo "BLOCKED: host not reachable over ssh" >&2
    exit 3
fi

# ---- 2. read-only readiness checks ----
phone_id="$(remote "cat /sys/bus/usb/devices/${PORT_Q}/idVendor 2>/dev/null | tr -d '\n'; echo -n ':'; cat /sys/bus/usb/devices/${PORT_Q}/idProduct 2>/dev/null | tr -d '\n'" || true)"
display_status="$(remote "for c in /sys/class/drm/card*-DP-1/status /sys/class/drm/card*-HDMI-A-1/status; do [ -f \"\$c\" ] && cat \"\$c\"; done" || true)"
adb_status="$(remote "command -v adb >/dev/null 2>&1 && adb devices 2>/dev/null | awk 'NR>1 && \$2==\"device\"{n++} END{print (n?n:0)}' || echo none" || true)"
policy="$(remote "test -f /etc/udev/rules.d/70-android-auto-receiver.rules && echo installed || echo absent" || true)"

echo "readiness: phone_id='$phone_id' policy='$policy'" >&2
echo "readiness: display='$display_status' adb='$adb_status'" >&2

if [[ -z "$phone_id" || "$phone_id" == ":" ]]; then
    record readiness BLOCKED "" "" "" "phone_absent_on_port"
    write_result "BLOCKED" "phone_absent_on_port"
    exit 3
fi
if [[ "$policy" != "installed" ]]; then
    record readiness BLOCKED "" "" "" "udev_policy_not_installed"
    write_result "BLOCKED" "udev_policy_not_installed"
    exit 3
fi
# Exact-token DRM status match (gate P2-C): `disconnected` must NOT read as
# `connected`. Only an exact `connected` status record passes the prerequisite.
display_connected="false"
for token in $display_status; do
    if [[ "$token" == "connected" ]]; then
        display_connected="true"
    fi
done
if [[ "$display_connected" != "true" ]]; then
    record readiness BLOCKED "" "" "" "display_disconnected"
    write_result "BLOCKED" "display_disconnected"
    exit 3
fi
if ((READINESS_ONLY)); then
    write_result "READY" ""
    exit 0
fi

if [[ -z "$RECEIVER" || -z "$DISPLAY" ]]; then
    record readiness BLOCKED "" "" "" "receiver_or_display_binary_not_supplied"
    write_result "BLOCKED" "receiver_or_display_binary_not_supplied"
    echo "BLOCKED: --receiver and --display are required for the recovery run" >&2
    exit 3
fi

# ---- 3. stack startup + initial projection (BEFORE every scenario) ----
# The receiver and display are started and VALIDATED (spawned PIDs alive) and
# an initial projecting state is observed before any scenario runs - including
# --only runs: a scenario must disturb an already-projecting stack, never race
# the startup (item 4).
trap cleanup_target EXIT

if ! start_receiver; then
    record setup BLOCKED "" "" "" "receiver_start_failed"
    write_result "BLOCKED" "receiver_start_failed"
    exit 3
fi
if ! start_consumer; then
    record setup BLOCKED "" "" "" "display_start_failed"
    write_result "BLOCKED" "display_start_failed"
    exit 3
fi
if ! proc_alive "${RECEIVER_PID}"; then
    record setup BLOCKED "" "" "" "receiver_not_running"
    write_result "BLOCKED" "receiver_not_running"
    exit 3
fi
if ! proc_alive "${CONSUMER_PID}"; then
    record setup BLOCKED "" "" "" "display_not_running"
    write_result "BLOCKED" "display_not_running"
    exit 3
fi
if ! wait_projecting "${BUDGET_MS}"; then
    record setup FAILED "" "" "" "no_initial_projection"
    write_result "FAILED" "setup:no_initial_projection"
    exit 1
fi

# ---- 4. run the scenario matrix ----
for scenario in "${SCENARIOS[@]}"; do
    if scenario_selected "$scenario"; then
        run_scenario "$scenario"
    fi
done

# Defense in depth (gate N3): a verdict over zero recorded rows reads PASS.
# Parse-time validation above makes this unreachable, but if the selection
# logic ever drifts from the matrix list again the run must fail closed.
if [[ ! -s "${RESULTS_TSV}" ]]; then
    echo "refusing empty scenario selection: no scenario row recorded; accepted values: ${SCENARIOS[*]}" >&2
    exit 2
fi

# ---- 4. verdict: PASS only if every SELECTED scenario passed with evidence ----
overall="PASS"
blocked_on=""
while IFS='|' read -r name status recovery retries evidence detail frames_baseline elapsed; do
    [[ -n "${name}" ]] || continue
    if [[ "$status" == PASS ]] && ! python3 -c 'import sys; v=float(sys.argv[1]); sys.exit(not (0 <= v < int(sys.argv[2])))' "$recovery" "$BUDGET_MS"; then
        status=FAILED detail=timing_error_pass_invariant
    fi
    case "${status}" in
        PASS|N/A) ;;
        BLOCKED) [[ "$overall" == FAILED ]] || overall="BLOCKED"; blocked_on="${blocked_on:+${blocked_on},}${name}:${detail}" ;;
        *) overall="FAILED"; blocked_on="${blocked_on:+${blocked_on},}${name}:${detail}" ;;
    esac
done < "${RESULTS_TSV}"

write_result "$overall" "$blocked_on"
if [[ -n "${ONLY}" ]]; then
    echo "verdict: ${overall} SUBSET (only=${ONLY}) - NOT a full-matrix pass" >&2
else
    echo "verdict: ${overall} (full matrix)" >&2
fi
case "${overall}" in
    PASS) exit 0 ;;
    BLOCKED) exit 3 ;;
    *) exit 1 ;;
esac
