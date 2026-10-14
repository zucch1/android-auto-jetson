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
        --only) ONLY="$2"; shift 2 ;;
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
if ! [[ "$EVIDENCE_WAIT_MS" =~ ^[0-9]+$ ]]; then
    echo "refusing hostile EVIDENCE_WAIT_MS value: must be a plain integer" >&2
    exit 2
fi

shq() { printf '%q' "$1"; }

# Bounded remote command (hung_commands class): 30 s hard ceiling per ssh.
# A batch ssh has no desktop session, so every remote command exports the
# session bus + display first: without them `busctl --user` fails with
# "Failed to get credentials: No such device or address" and the Qt display
# consumer aborts with no DISPLAY. $(id -u) must expand on the TARGET (escaped
# here); it is the ssh login user's uid, not this host's.
remote() {
    timeout 30 ssh "${SSH_OPTS[@]}" -- "$HOST" \
        "export DBUS_SESSION_BUS_ADDRESS=\"unix:path=/run/user/\$(id -u)/bus\"; export DISPLAY=:1; $*"
}

json_escape() { printf '%s' "$1" | tr -d '\000' | sed 's/\\/\\\\/g; s/"/\\"/g'; }

host_tag="$(printf '%s' "$HOST" | sha256sum | cut -c1-16)"
PORT_Q="$(shq "$PORT")"

# Append one scenario record: name, status, recovery_ms (or ""), retries (or ""),
# evidence, detail. '|' (not tab) is the field separator: tab is IFS whitespace
# and collapses the empty recovery/retries fields, which shifted values into the
# wrong JSON slots.
record() {
    printf '%s|%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5" "$6" >> "${RESULTS_TSV}"
}

# D-Bus projection state (the same contract the task-30 harness uses).
projecting() {
    local state
    state="$(remote "busctl --user call org.custom.AndroidAutoReceiver /org/custom/AndroidAutoReceiver org.custom.AndroidAutoReceiver1 GetState 2>/dev/null" || true)"
    [[ "${state}" == *projecting* ]]
}

wait_projecting() {
    local deadline=$(( $(date +%s%3N) + $1 ))
    while (( $(date +%s%3N) < deadline )); do
        if projecting; then
            return 0
        fi
        sleep 0.2
    done
    return 1
}

wait_not_projecting() {
    local deadline=$(( $(date +%s%3N) + $1 ))
    while (( $(date +%s%3N) < deadline )); do
        if ! projecting; then
            return 0
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

retry_count() {
    local log="${1:-/tmp/aa-wired-receiver.log}"
    remote "grep -o '\"retries\":[0-9]*' ${log} 2>/dev/null | grep -o '[0-9]*' | sort -n | tail -1" || true
}

# ---- observed-evidence probes (gate P2: PASS requires OBSERVED evidence) ----

# Painted-frames evidence from the SHIPPED dummy display's test-only status
# channel (task 24 testcontrol): frames_painted > 0 and render_ok true. This is
# the display-side "presents frames" claim - video_sent alone only proves units
# crossed the channel, not that anything painted.
painted_frames() {
    local line
    remote "printf 'status\n' > /tmp/aa-disp.in 2>/dev/null; sleep 0.3; grep '\"event\":\"status\"' /tmp/aa-wired-display.log 2>/dev/null | tail -1" >/tmp/aa-status.$$ 2>/dev/null || true
    line="$(cat /tmp/aa-status.$$ 2>/dev/null || true)"
    rm -f /tmp/aa-status.$$
    [[ "$line" =~ \"frames_painted\":([0-9]+) ]] || return 1
    (( ${BASH_REMATCH[1]} > 0 )) || return 1
    [[ "$line" == *'"render_ok":true'* ]]
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
rejection_observed() {
    local since="${1:-0}"
    local window
    window="$(log_since "${since}")"
    [[ "$window" == *transport_unauthorized_peer* ]] && return 0
    [[ "$window" == *session_unknown_phone* ]] && return 0
    [[ "$window" == *'"code":"302"'* ]] && return 0
    [[ "$window" == *connect-failed* ]] && return 0
    return 1
}

# Full media evidence required before any recovery PASS: painted frames AND a
# successful audio pump AND a delivered input event (stimulated here).
media_evidence() {
    local since="${1:-0}"
    stimulate_input
    local deadline=$(( $(date +%s%3N) + EVIDENCE_WAIT_MS ))
    while (( $(date +%s%3N) < deadline )); do
        if painted_frames && audio_pumped "${since}" && input_ok "${since}"; then
            return 0
        fi
        sleep 0.5
    done
    return 1
}

start_receiver() {
    [[ -n "${RECEIVER}" ]] || return 1
    local args="--port ${PORT_Q}"
    [[ -n "${STORE}" ]] && args="${args} --store $(shq "$STORE")"
    remote "nohup $(shq "$RECEIVER") ${args} > /tmp/aa-wired-receiver.log 2>&1 & echo \$!" >/dev/null 2>&1
}

start_consumer() {
    [[ -n "${DISPLAY}" ]] || return 1
    # The display runs with the shipped test-only stdin control channel open on
    # a FIFO (<> keeps the open non-blocking): it is the painted-frames and
    # input-stimulation evidence source for the PASS gates.
    remote "mkfifo /tmp/aa-disp.in 2>/dev/null || true; nohup $(shq "$DISPLAY") --test-only-autostart --test-only-control-stdin <>/tmp/aa-disp.in > /tmp/aa-wired-display.log 2>&1 & echo \$!" >/dev/null 2>&1
}

scenario_selected() {
    [[ -z "${ONLY}" || ",${ONLY}," == *",$1,"* ]]
}

# ---- scenario implementations (each records exactly one row) ----
# PASS is earned only with observed evidence: stimulus outcome + disturbance
# (dip/rejection) + recovery inside the budget window (bounded-wait evidence)
# + real media delivery. Anything unobservable is BLOCKED, never PASS.

scenario_receiver_restart() {
    local t0 t1 recovery offset
    start_receiver || { record receiver_restart BLOCKED "" "" "" "receiver_binary_not_supplied"; return; }
    if ! wait_projecting "${BUDGET_MS}"; then
        record receiver_restart FAILED "" "" "" "no_initial_projection"
        return
    fi
    remote "pkill -f 'aa-wired-receiver' || true" >/dev/null 2>&1 || true
    sleep 1
    # The receiver's process death is the observed state dip (gate P2).
    if remote "pgrep -f 'aa-wired-receiver' >/dev/null 2>&1 && echo alive || echo gone" 2>/dev/null | grep -q alive; then
        record receiver_restart FAILED "" "" "dip_unobserved" "receiver_did_not_stop"
        return
    fi
    wait_not_projecting 3000 || true
    t0=$(date +%s%3N)
    offset="$(log_offset)"
    start_receiver || { record receiver_restart FAILED "" "" "" "restart_failed"; return; }
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        if ! remote "grep -q '\"event\":\"active\"' /tmp/aa-wired-receiver.log 2>/dev/null && echo ok" >/dev/null 2>&1; then
            record receiver_restart FAILED "${recovery}" "$(retry_count)" "session_active_not_observed" "no_active_event"
            return
        fi
        if media_evidence "${offset}"; then
            record receiver_restart PASS "${recovery}" "$(retry_count)" "process_dip+active+frames+pump+input" "recovered"
        else
            record receiver_restart FAILED "${recovery}" "$(retry_count)" "process_dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record receiver_restart FAILED "" "$(retry_count)" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_consumer_restart() {
    local t0 t1 recovery offset
    start_consumer || { record consumer_restart BLOCKED "" "" "" "display_binary_not_supplied"; return; }
    wait_projecting "${BUDGET_MS}" || { record consumer_restart FAILED "" "" "" "no_initial_projection"; return; }
    remote "pkill -f 'aa-dummy-display' || true" >/dev/null 2>&1 || true
    sleep 1
    # The consumer's process death is the observed state dip (gate P2).
    if remote "pgrep -f 'aa-dummy-display' >/dev/null 2>&1 && echo alive || echo gone" 2>/dev/null | grep -q alive; then
        record consumer_restart FAILED "" "" "dip_unobserved" "display_did_not_stop"
        return
    fi
    t0=$(date +%s%3N)
    offset="$(log_offset)"
    start_consumer || { record consumer_restart FAILED "" "" "" "restart_failed"; return; }
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        if media_evidence "${offset}"; then
            record consumer_restart PASS "${recovery}" "$(retry_count)" "process_dip+frames+pump+input" "recovered"
        else
            record consumer_restart FAILED "${recovery}" "$(retry_count)" "process_dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record consumer_restart FAILED "" "$(retry_count)" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_usb_unplug_replug() {
    local t0 t1 recovery offset
    wait_projecting "${BUDGET_MS}" || { record usb_unplug_replug FAILED "" "" "" "no_initial_projection"; return; }
    offset="$(log_offset)"
    if ! remote "sudo -n sh -c 'echo -n ${PORT_Q} > /sys/bus/usb/drivers/usb/unbind'" >/dev/null 2>&1; then
        record usb_unplug_replug BLOCKED "" "" "" "sysfs_unbind_requires_passwordless_sudo"
        return
    fi
    sleep 1
    t0=$(date +%s%3N)
    if ! remote "sudo -n sh -c 'echo -n ${PORT_Q} > /sys/bus/usb/drivers/usb/bind'" >/dev/null 2>&1; then
        record usb_unplug_replug BLOCKED "" "" "" "sysfs_bind_requires_passwordless_sudo"
        return
    fi
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        if ! dip_observed "${offset}"; then
            record usb_unplug_replug BLOCKED "${recovery}" "$(retry_count)" "dip_unobserved" "no_state_or_flow_dip_observed"
            return
        fi
        if media_evidence "${offset}"; then
            record usb_unplug_replug PASS "${recovery}" "$(retry_count)" "dip+frames+pump+input" "recovered_on_replug"
        else
            record usb_unplug_replug FAILED "${recovery}" "$(retry_count)" "dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record usb_unplug_replug FAILED "" "$(retry_count)" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_phone_lock_unlock() {
    local t0 t1 recovery offset
    wait_projecting "${BUDGET_MS}" || { record phone_lock_unlock FAILED "" "" "" "no_initial_projection"; return; }
    offset="$(log_offset)"
    remote "adb ${ADB_SERIAL:+-s $(shq "$ADB_SERIAL")} shell input keyevent 26" >/dev/null 2>&1 || true
    sleep 2
    t0=$(date +%s%3N)
    remote "adb ${ADB_SERIAL:+-s $(shq "$ADB_SERIAL")} shell input keyevent 224" >/dev/null 2>&1 || true
    remote "adb ${ADB_SERIAL:+-s $(shq "$ADB_SERIAL")} shell input swipe 540 1600 540 400" >/dev/null 2>&1 || true
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        if ! dip_observed "${offset}"; then
            record phone_lock_unlock BLOCKED "${recovery}" "$(retry_count)" "dip_unobserved" "no_state_or_flow_dip_observed"
            return
        fi
        if media_evidence "${offset}"; then
            record phone_lock_unlock PASS "${recovery}" "$(retry_count)" "dip+frames+pump+input" "recovered_after_unlock"
        else
            record phone_lock_unlock FAILED "${recovery}" "$(retry_count)" "dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record phone_lock_unlock FAILED "" "$(retry_count)" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_phone_aa_restart() {
    local t0 t1 recovery offset
    wait_projecting "${BUDGET_MS}" || { record phone_aa_restart FAILED "" "" "" "no_initial_projection"; return; }
    offset="$(log_offset)"
    t0=$(date +%s%3N)
    remote "adb ${ADB_SERIAL:+-s $(shq "$ADB_SERIAL")} shell am force-stop com.google.android.projection.gearhead" >/dev/null 2>&1 || true
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        if ! dip_observed "${offset}"; then
            record phone_aa_restart BLOCKED "${recovery}" "$(retry_count)" "dip_unobserved" "no_state_or_flow_dip_observed"
            return
        fi
        if media_evidence "${offset}"; then
            record phone_aa_restart PASS "${recovery}" "$(retry_count)" "dip+frames+pump+input" "recovered_after_aa_restart"
        else
            record phone_aa_restart FAILED "${recovery}" "$(retry_count)" "dip+media_unobserved" "no_media_evidence_after_recovery"
        fi
    else
        record phone_aa_restart FAILED "" "$(retry_count)" "not_recovered" "not_recovered_within_budget"
    fi
}

scenario_unknown_phone() {
    # A second, unapproved device must never reach projection, and the rejection
    # must be OBSERVED (admission/link failure telemetry) - a quiet log is not
    # evidence of a rejection, it is the absence of one.
    local devices count offset
    devices="$(remote "adb devices 2>/dev/null | awk 'NR>1 && \$2==\"device\"{print \$1}'" || true)"
    count="$(printf '%s\n' "${devices}" | grep -c . || true)"
    if (( count < 2 )); then
        record unknown_phone BLOCKED "" "" "" "second_device_absent_phase_a_fixture_covers_logic"
        return
    fi
    offset="$(log_offset)"
    remote "adb ${ADB_SERIAL:+-s $(shq "$ADB_SERIAL")} shell true" >/dev/null 2>&1 || true
    local deadline=$(( $(date +%s%3N) + EVIDENCE_WAIT_MS ))
    while (( $(date +%s%3N) < deadline )); do
        if rejection_observed "${offset}"; then
            if projecting; then
                record unknown_phone FAILED "" "" "rejection+projection" "unknown_device_present_while_projecting"
            else
                record unknown_phone PASS "" "" "rejection_observed" "unknown_device_rejected_fail_closed"
            fi
            return
        fi
        sleep 0.5
    done
    record unknown_phone BLOCKED "" "" "rejection_unobserved" "no_rejection_observed_cannot_claim_fail_closed"
}

scenario_malformed_input() {
    record malformed_input N/A "" "" "" "host_covered_by_ctest_wired_recovery_no_shell_injector"
}

cleanup_target() {
    remote "pkill -f 'aa-dummy-display' || true; pkill -f 'aa-wired-receiver' || true" >/dev/null 2>&1 || true
}

write_result() {
    local overall_status="$1" blocked_on="$2"
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
        printf '  "only": %s,\n' "${only_field}"
        printf '  "subset": %s,\n' "${subset}"
        printf '  "status": "%s",\n' "$overall_status"
        printf '  "blocked_on": "%s",\n' "$(json_escape "$blocked_on")"
        printf '  "scenarios": [\n'
        local first=1 name status recovery retries evidence detail
        while IFS='|' read -r name status recovery retries evidence detail; do
            [[ -n "${name}" ]] || continue
            (( first )) || printf ',\n'
            first=0
            printf '    {"name": "%s", "status": "%s", "recovery_ms": %s, "retries": %s, "evidence": "%s", "detail": "%s"}' \
                "$(json_escape "$name")" "$(json_escape "$status")" \
                "${recovery:-null}" "${retries:-null}" \
                "$(json_escape "$evidence")" "$(json_escape "$detail")"
        done < "${RESULTS_TSV}"
        printf '\n  ],\n'
        printf '  "budget_note": "recovery_ms values are self-measured wall times and are UNMEASURED for budget purposes; the <=%s ms budget claim rests solely on each scenario bounded wait (the harness own timeout evidence)",\n' "$BUDGET_MS"
        printf '  "note": "%s"\n' "$(json_escape "${verdict_note}")"
        printf '}\n'
    } > "$OUT"
    echo "wrote $OUT (status=$overall_status${blocked_on:+ blocked_on=$blocked_on})"
}

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

# ---- 3. run the scenario matrix ----
trap cleanup_target EXIT

for scenario in usb_unplug_replug receiver_restart consumer_restart \
                phone_lock_unlock phone_aa_restart unknown_phone malformed_input; do
    if scenario_selected "$scenario"; then
        "scenario_${scenario}"
    fi
done

# ---- 4. verdict: PASS only if every SELECTED scenario passed with evidence ----
overall="PASS"
blocked_on=""
while IFS='|' read -r name status recovery retries evidence detail; do
    [[ -n "${name}" ]] || continue
    case "${status}" in
        PASS|N/A) ;;
        BLOCKED) overall="BLOCKED"; blocked_on="${blocked_on:+${blocked_on},}${name}:${detail}" ;;
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
