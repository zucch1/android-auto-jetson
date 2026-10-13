#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Task 31 phase-B wired reconnect/recovery live-QA harness (READY TO RUN). This
# is the live companion to the host-side logic matrix (`ctest -R wired_recovery`):
# it drives the REAL approved phone + on-target wired receiver through the plan's
# recovery scenarios and records per-scenario recovery time and retry counts.
#
# It is deliberately NOT run in phase A: the Jetson display is DP-1 DISCONNECTED,
# the on-target receiver binary is not yet installable (aa_protocol_adapter is
# native-only; task-41 scope), and the task-29 udev policy was ROLLED BACK after
# its own QA round (reinstall as prerequisite 1 before a run). No live recovery
# claim is made until an invocation writes a PASS record.
#
# Fail-closed by construction: every ssh runs under `timeout`; a missing
# prerequisite writes a BLOCKED result and exits non-zero; exit 0 requires every
# executable scenario to observe recovery (or a clean fail-closed) within the
# wired 5 s budget. A green smoke without an observed recovery is never a pass.
#
# Scenarios:
#   usb_unplug_replug  sysfs unbind/bind the phone's USB device on the port
#                      (bounded and reversible; the phone stays functional)
#   receiver_restart   bounce the on-target receiver process
#   consumer_restart   bounce the dummy consumer (display) process
#   phone_lock_unlock  ADB keyevent lock + wake/unlock
#   phone_aa_restart   ADB force-stop Android Auto (phone-side restart)
#   unknown_phone      a second (S9 Edge) device must fail closed; BLOCKED when
#                      absent (the phase-A synthetic fixture covers the logic)
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
#       --out /tmp/task-31-wired-recovery.json
#
# Environment overrides: HOST (jetson.local), PORT (1-2.1), BUDGET_MS (5000).

set -euo pipefail

HOST="${HOST:-jetson.local}"
PORT="${PORT:-1-2.1}"
RECEIVER=""
DISPLAY=""
STORE=""
ADB_SERIAL=""
OUT="/tmp/task-31-wired-recovery.json"
BUDGET_MS="${BUDGET_MS:-5000}"
READINESS_ONLY=0
ONLY=""
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=5 -o ServerAliveCountMax=2)
RESULTS_DIR="$(mktemp -d)"
RESULTS_TSV="${RESULTS_DIR}/results.tsv"
: > "${RESULTS_TSV}"

usage() {
    sed -n '2,52p' "$0"
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

# Bounded remote command (hung_commands class): 30 s hard ceiling per ssh.
remote() { timeout 30 ssh "${SSH_OPTS[@]}" "$HOST" "$@"; }

json_escape() { printf '%s' "$1" | tr -d '\000' | sed 's/\\/\\\\/g; s/"/\\"/g'; }

# Append one scenario record: name, status, recovery_ms (or ""), retries (or ""), detail.
# '|' (not tab) is the field separator: tab is IFS whitespace and collapses the
# empty recovery/retries fields, which shifted values into the wrong JSON slots.
record() {
    printf '%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5" >> "${RESULTS_TSV}"
}

# D-Bus projection state (the same contract the task-30 harness uses).
projecting() {
    local state
    state="$(remote "busctl --user call org.custom.AndroidAutoReceiver /org/custom/AndroidAutoReceiver org.custom.AndroidAutoReceiver1 GetState 2>/dev/null" || true)"
    [[ "${state}" == *projecting* ]]
}

# Wait until projection is observed; $1 = timeout ms. Returns 1 on timeout.
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

# Wait until projection is NOT observed; $1 = timeout ms.
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

# Highest reconnect retry count reported by the receiver diagnostics log.
retry_count() {
    local log="${1:-/tmp/aa-wired-receiver.log}"
    remote "grep -o '\"retries\":[0-9]*' ${log} 2>/dev/null | grep -o '[0-9]*' | sort -n | tail -1" || true
}

start_receiver() {
    [[ -n "${RECEIVER}" ]] || return 1
    local args="--port ${PORT}"
    [[ -n "${STORE}" ]] && args="${args} --store ${STORE}"
    remote "nohup ${RECEIVER} ${args} > /tmp/aa-wired-receiver.log 2>&1 & echo \$!" >/dev/null 2>&1
}

start_consumer() {
    [[ -n "${DISPLAY}" ]] || return 1
    remote "nohup ${DISPLAY} --test-only-autostart > /tmp/aa-wired-display.log 2>&1 & echo \$!" >/dev/null 2>&1
}

scenario_selected() {
    [[ -z "${ONLY}" || ",${ONLY}," == *",$1,"* ]]
}

# ---- scenario implementations (each records exactly one row) ----

scenario_receiver_restart() {
    local t0 t1 recovery
    start_receiver || { record receiver_restart BLOCKED "" "" "receiver_binary_not_supplied"; return; }
    if ! wait_projecting "${BUDGET_MS}"; then
        record receiver_restart FAILED "" "$(retry_count)" "no_initial_projection"
        return
    fi
    remote "pkill -f 'aa-wired-receiver' || true" >/dev/null 2>&1 || true
    wait_not_projecting 3000 || true
    t0=$(date +%s%3N)
    start_receiver || { record receiver_restart FAILED "" "" "restart_failed"; return; }
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        record receiver_restart PASS "${recovery}" "$(retry_count)" "recovered"
    else
        record receiver_restart FAILED "" "$(retry_count)" "not_recovered_within_budget"
    fi
}

scenario_consumer_restart() {
    local t0 t1 recovery
    start_consumer || { record consumer_restart BLOCKED "" "" "display_binary_not_supplied"; return; }
    wait_projecting "${BUDGET_MS}" || { record consumer_restart FAILED "" "$(retry_count)" "no_initial_projection"; return; }
    remote "pkill -f 'aa-dummy-display' || true" >/dev/null 2>&1 || true
    wait_not_projecting 3000 || true
    t0=$(date +%s%3N)
    start_consumer || { record consumer_restart FAILED "" "" "restart_failed"; return; }
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        record consumer_restart PASS "${recovery}" "$(retry_count)" "recovered"
    else
        record consumer_restart FAILED "" "$(retry_count)" "not_recovered_within_budget"
    fi
}

scenario_usb_unplug_replug() {
    local t0 t1 recovery
    wait_projecting "${BUDGET_MS}" || { record usb_unplug_replug FAILED "" "$(retry_count)" "no_initial_projection"; return; }
    if ! remote "sudo -n sh -c 'echo -n ${PORT} > /sys/bus/usb/drivers/usb/unbind'" >/dev/null 2>&1; then
        record usb_unplug_replug BLOCKED "" "" "sysfs_unbind_requires_passwordless_sudo"
        return
    fi
    sleep 1
    t0=$(date +%s%3N)
    if ! remote "sudo -n sh -c 'echo -n ${PORT} > /sys/bus/usb/drivers/usb/bind'" >/dev/null 2>&1; then
        record usb_unplug_replug BLOCKED "" "" "sysfs_bind_requires_passwordless_sudo"
        return
    fi
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        record usb_unplug_replug PASS "${recovery}" "$(retry_count)" "recovered_on_replug"
    else
        record usb_unplug_replug FAILED "" "$(retry_count)" "not_recovered_within_budget"
    fi
}

scenario_phone_lock_unlock() {
    local t0 t1 recovery
    wait_projecting "${BUDGET_MS}" || { record phone_lock_unlock FAILED "" "$(retry_count)" "no_initial_projection"; return; }
    remote "adb ${ADB_SERIAL:+-s ${ADB_SERIAL}} shell input keyevent 26" >/dev/null 2>&1 || true
    sleep 2
    t0=$(date +%s%3N)
    remote "adb ${ADB_SERIAL:+-s ${ADB_SERIAL}} shell input keyevent 224" >/dev/null 2>&1 || true
    remote "adb ${ADB_SERIAL:+-s ${ADB_SERIAL}} shell input swipe 540 1600 540 400" >/dev/null 2>&1 || true
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        record phone_lock_unlock PASS "${recovery}" "$(retry_count)" "recovered_after_unlock"
    else
        record phone_lock_unlock FAILED "" "$(retry_count)" "not_recovered_within_budget"
    fi
}

scenario_phone_aa_restart() {
    local t0 t1 recovery
    wait_projecting "${BUDGET_MS}" || { record phone_aa_restart FAILED "" "$(retry_count)" "no_initial_projection"; return; }
    t0=$(date +%s%3N)
    remote "adb ${ADB_SERIAL:+-s ${ADB_SERIAL}} shell am force-stop com.google.android.projection.gearhead" >/dev/null 2>&1 || true
    if wait_projecting "${BUDGET_MS}"; then
        t1=$(date +%s%3N); recovery=$(( t1 - t0 ))
        record phone_aa_restart PASS "${recovery}" "$(retry_count)" "recovered_after_aa_restart"
    else
        record phone_aa_restart FAILED "" "$(retry_count)" "not_recovered_within_budget"
    fi
}

scenario_unknown_phone() {
    # A second, unapproved device must never reach projection.
    local devices
    devices="$(remote "adb devices 2>/dev/null | awk 'NR>1 && \$2==\"device\"{print \$1}'" || true)"
    local count
    count="$(printf '%s\n' "${devices}" | grep -c . || true)"
    if (( count < 2 )); then
        record unknown_phone BLOCKED "" "" "second_device_absent_phase_a_fixture_covers_logic"
        return
    fi
    if projecting; then
        record unknown_phone FAILED "" "" "unknown_device_present_while_projecting"
    else
        record unknown_phone PASS "" "" "unknown_device_fails_closed"
    fi
}

scenario_malformed_input() {
    record malformed_input N/A "" "" "host_covered_by_ctest_wired_recovery_no_shell_injector"
}

cleanup_target() {
    remote "pkill -f 'aa-dummy-display' || true; pkill -f 'aa-wired-receiver' || true" >/dev/null 2>&1 || true
}

write_result() {
    local overall_status="$1" blocked_on="$2"
    mkdir -p "$(dirname "$OUT")"
    {
        printf '{\n'
        printf '  "task": "31. Implement wired reconnect and recovery matrix",\n'
        printf '  "phase": "B (live device)",\n'
        printf '  "host": "%s",\n' "$(json_escape "$HOST")"
        printf '  "port": "%s",\n' "$(json_escape "$PORT")"
        printf '  "budget_ms": %s,\n' "$BUDGET_MS"
        printf '  "status": "%s",\n' "$overall_status"
        printf '  "blocked_on": "%s",\n' "$(json_escape "$blocked_on")"
        printf '  "scenarios": [\n'
        local first=1 name status recovery retries detail
        while IFS='|' read -r name status recovery retries detail; do
            [[ -n "${name}" ]] || continue
            (( first )) || printf ',\n'
            first=0
            printf '    {"name": "%s", "status": "%s", "recovery_ms": %s, "retries": %s, "detail": "%s"}' \
                "$(json_escape "$name")" "$(json_escape "$status")" \
                "${recovery:-null}" "${retries:-null}" "$(json_escape "$detail")"
        done < "${RESULTS_TSV}"
        printf '\n  ],\n'
        printf '  "note": "phase-A host-side acceptance is separate (ctest -R wired_recovery); this is the live record"\n'
        printf '}\n'
    } > "$OUT"
    echo "wrote $OUT (status=$overall_status${blocked_on:+ blocked_on=$blocked_on})"
}

# ---- 1. reachability (bounded) ----
if ! remote "true" >/dev/null 2>&1; then
    record ssh BLOCKED "" "" "ssh_unreachable"
    write_result "BLOCKED" "ssh_unreachable"
    echo "BLOCKED: $HOST not reachable over ssh" >&2
    exit 3
fi

# ---- 2. read-only readiness checks ----
phone_id="$(remote "cat /sys/bus/usb/devices/${PORT}/idVendor 2>/dev/null | tr -d '\n'; echo -n ':'; cat /sys/bus/usb/devices/${PORT}/idProduct 2>/dev/null | tr -d '\n'" || true)"
display_status="$(remote "for c in /sys/class/drm/card*-DP-1/status /sys/class/drm/card*-HDMI-A-1/status; do [ -f \"\$c\" ] && { basename \"\$(dirname \"\$c\")\"; cat \"\$c\"; }; done | head -2" || true)"
adb_status="$(remote "command -v adb >/dev/null 2>&1 && adb devices 2>/dev/null | awk 'NR>1 && \$2==\"device\"{n++} END{print (n?n:0)}' || echo none" || true)"
policy="$(remote "test -f /etc/udev/rules.d/70-android-auto-receiver.rules && echo installed || echo absent" || true)"

echo "readiness: phone_id='$phone_id' policy='$policy'" >&2
echo "readiness: display='$display_status' adb='$adb_status'" >&2

if [[ -z "$phone_id" || "$phone_id" == ":" ]]; then
    record readiness BLOCKED "" "" "phone_absent_on_port"
    write_result "BLOCKED" "phone_absent_on_port"
    exit 3
fi
if [[ "$policy" != "installed" ]]; then
    record readiness BLOCKED "" "" "udev_policy_not_installed"
    write_result "BLOCKED" "udev_policy_not_installed"
    exit 3
fi
if [[ ! "${display_status}" = *connected* ]]; then
    record readiness BLOCKED "" "" "display_disconnected"
    write_result "BLOCKED" "display_disconnected"
    exit 3
fi
if ((READINESS_ONLY)); then
    write_result "READY" ""
    exit 0
fi

if [[ -z "$RECEIVER" || -z "$DISPLAY" ]]; then
    record readiness BLOCKED "" "" "receiver_or_display_binary_not_supplied"
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

# ---- 4. verdict: PASS only if every executable scenario passed ----
overall="PASS"
blocked_on=""
while IFS='|' read -r name status recovery retries detail; do
    [[ -n "${name}" ]] || continue
    case "${status}" in
        PASS|N/A) ;;
        BLOCKED) overall="BLOCKED"; blocked_on="${blocked_on:+${blocked_on},}${name}:${detail}" ;;
        *) overall="FAILED"; blocked_on="${blocked_on:+${blocked_on},}${name}:${detail}" ;;
    esac
done < "${RESULTS_TSV}"

write_result "$overall" "$blocked_on"
[[ "$overall" == "PASS" ]]
