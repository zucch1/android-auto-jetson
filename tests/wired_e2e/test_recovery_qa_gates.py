#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wired_e2e/test_recovery_qa_gates.py <path-to-run-wired-recovery-qa.sh>
"""Script-level gate regressions for tools/wired-qa/run-wired-recovery-qa.sh.

A fake `ssh` shim is placed on PATH and scripted through environment variables so
the recovery harness's readiness + scenario + verdict logic runs hermetically (no
device). Each case asserts the exact JSON verdict and exit code:

  P2-C a DRM status of `disconnected` must fail display readiness; only an exact
       `connected` token passes.
  P2   every scenario needs OBSERVED evidence: a recovery without a state/flow
       dip is BLOCKED (never PASS), and an unknown phone without an observed
       rejection is BLOCKED (never PASS).
  P2-G a GetState=projecting string with no media (painted frames / successful
       audio pump / "ok":"true" input) is NON-PASS, and each of the three
       media conjuncts is pinned independently (N1).
  P3   a --only subset run records `only` + `subset` and marks the verdict text;
       hostile HOST (leading dash) and PORT (quote break) are neutralized; the
       host is never written raw; the budget claim is labelled unmeasured.
  N3   an empty, unknown or malformed --only selection is a usage error naming
       the accepted values - never a vacuous PASS over zero scenarios.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

TIMEOUT_S = 90

FAKE_SSH = r"""#!/usr/bin/env bash
set -euo pipefail
trap 'rc=$?; printf "%s|%s\n" "$rc" "$cmd" >> "$FAKE_OUTCOMES"' EXIT
if [ -n "${FAKE_SSH_LOG:-}" ]; then
    printf '%s\n' "$*" >> "$FAKE_SSH_LOG"
fi
cmd="${@: -1}"
all_args="$*"
state="${FAKE_SSH_STATE}"
mkdir -p "$state"
stimulus=0
case "$cmd" in
    *'kill 4242'*|*'kill 4243'*|*unbind*|*'keyevent 26'*|*'am force-stop'*|*'--no-bus --duration-ms'*) stimulus=1 ;;
esac
if (( stimulus )) && [ ! -f "$state/stimulus" ]; then
    touch "$state/stimulus"
    [ -z "${FAKE_FIRST_DELAY:-}" ] || sleep "$FAKE_FIRST_DELAY"
fi
case "$cmd" in
    *'kill 4242'*|*'kill 4243'*) [ -z "${FAKE_CLEANUP_DELAY:-}" ] || sleep "$FAKE_CLEANUP_DELAY" ;;
esac
if [ -f "$state/stimulus" ]; then
    case "$cmd" in
        *busctl*) [ -z "${FAKE_POST_STATE_DELAY:-}" ] || sleep "$FAKE_POST_STATE_DELAY" ;;
        *'--no-bus --duration-ms'*) [ -z "${FAKE_ATTEMPT_DELAY:-}" ] || sleep "$FAKE_ATTEMPT_DELAY" ;;
        *'tail -c +1 /tmp/aa-wired-receiver-attempt.log'*) [ -z "${FAKE_ATTEMPT_LOG_DELAY:-}" ] || sleep "$FAKE_ATTEMPT_LOG_DELAY" ;;
        *'"event":"status"'*)
            if [ -f "$state/frames_n" ]; then
                [ -z "${FAKE_MEDIA_DELAY:-}" ] || sleep "$FAKE_MEDIA_DELAY"
            fi ;;
    esac
    [ -z "${FAKE_COMMAND_DELAY:-}" ] || sleep "$FAKE_COMMAND_DELAY"
fi
if [ -n "${FAKE_TERMINAL_MS:-}" ]; then
    case "$cmd" in
        *'"event":"input_forwarded","ok":"true"'*) printf '3' > "$state/terminal_countdown" ;;
        *'tail -c +1 /tmp/aa-wired-receiver-attempt.log'*) printf '2' > "$state/terminal_countdown" ;;
    esac
fi
case "$all_args" in
    *'DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"'*|*'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus'*) ;;
    *) echo "fake-ssh: remote command missing target bus address: $all_args" >&2; exit 98 ;;
esac
case "$all_args" in
    *'DISPLAY=:1'*) ;;
    *) echo "fake-ssh: remote command missing DISPLAY=:1: $all_args" >&2; exit 98 ;;
esac
case "$cmd" in
    *'; true')
        [ "${FAKE_SSH_FAIL:-0}" = "1" ] && exit 255
        exit 0 ;;
esac
case "$cmd" in
    *idVendor*|*idProduct*) printf '%s' "${FAKE_PHONE:-04e8:6860}"; exit 0 ;;
    *"/sys/class/drm"*) printf '%s\n' "${FAKE_DISPLAY:-connected}"; exit 0 ;;
    *70-android-auto-receiver.rules*) printf '%s' "${FAKE_POLICY:-installed}"; exit 0 ;;
    *'adb devices -l'*) printf '%s\n' "${FAKE_ADB_LONG:-FAKEAPPROVED device usb:1-2.1 product:fake model:FakeApproved device:fake transport_id:1
FAKEDEV2 device usb:1-2.3 product:fake model:FakeS9Edge device:fake transport_id:2}"; exit 0 ;;
    *'n++'*) printf '%s' "${FAKE_ADB_COUNT:-2}"; exit 0 ;;
    *'print $1'*) printf '%s\n' "${FAKE_ADB_LIST:-FAKEDEV1 FAKEDEV2}" | tr ' ' '\n'; exit 0 ;;
    *'adb shell'*) exit 0 ;;
    *'shell input keyevent'*|*'shell input swipe'*|*'shell am force-stop'*) exit "${FAKE_ADB_STIMULUS_RC:-0}" ;;
    *'wc -c'*) printf '%s' "${FAKE_LOG_SIZE:-0}"; exit 0 ;;
    # Media-gate probes (N1): both probe commands contain their evidence token
    # AND `tail -c +`, so first-match case ordering decides which arm drives
    # them. The exact-shape arms MUST precede the raw-window tail arm (the
    # tail arm also serves dip_observed/rejection_observed window reads, which
    # carry no media tokens). The wrong-marker arms (audio_chunks / bare
    # input_forwarded) model the pre-P2-G defect shape: a gate that regressed
    # to presence greps lands there, so the per-conjunct pins can prove it.
    *'"event":"media_timing"'*'"event":"audio_chunks"'*)
        [ "${FAKE_MEDIA_TIMING:-0}" = "1" ] && [ "${FAKE_AUDIO_CHUNKS:-1}" = "1" ] && exit 0 || exit 1 ;;
    *'"event":"media_timing"'*) [ "${FAKE_MEDIA_TIMING:-0}" = "1" ] && exit 0 || exit 1 ;;
    *'"event":"input_forwarded","ok":"true"'*) [ "${FAKE_INPUT_OK:-0}" = "1" ] && exit 0 || exit 1 ;;
    *audio_chunks*) [ "${FAKE_AUDIO_CHUNKS:-1}" = "1" ] && exit 0 || exit 1 ;;
    *'input_forwarded'*) [ "${FAKE_INPUT_ANY:-0}" = "1" ] && exit 0 || exit 1 ;;
    *'tail -c +'*aa-wired-receiver-attempt.log*) printf '%s' "${FAKE_ATTEMPT_LOG:-}"; exit 0 ;;
    *'tail -c +'*) printf '%s' "${FAKE_LOG_WINDOW:-}"; exit 0 ;;
    *aa-wired-receiver-attempt.log*) exit "${FAKE_ATTEMPT_RC:-3}" ;;
    *'retries'*)
        n="$(cat "${FAKE_SSH_STATE:-/tmp}/retries_n" 2>/dev/null || echo 0)"
        n=$((n+1)); mkdir -p "${FAKE_SSH_STATE:-/tmp}"; echo "$n" > "${FAKE_SSH_STATE:-/tmp}/retries_n"
        printf '%s' "$(( ${FAKE_RETRIES_BASE:-${FAKE_RETRIES:-1}} + ${FAKE_RETRIES_STEP:-0} * (n-1) ))"
        exit 0 ;;
    *busctl*)
        [ -n "${FAKE_STATE_DELAY:-}" ] && sleep "${FAKE_STATE_DELAY}"
        if [ -f "$state/stimulus" ] && [ ! -f "$state/recv" ]; then echo 'string "stopped"'; else printf '%s' "${FAKE_STATE:-string \"projecting\"}"; fi
        exit 0 ;;
    # pgrep -f REAL matching semantics: the remote command line contains the
    # pattern (the harness quotes it into the ssh command), so pgrep -f matches
    # its own wrapper shell and ALWAYS reports alive. Death verification that
    # trusts pgrep -f is wrong by construction - the regression model below.
    *pgrep*) echo alive; exit 0 ;;
    *pkill*) exit 0 ;;
    *'kill -0 4242'*) [ "${FAKE_PROC_ALIVE:-1}" = "0" ] && { echo gone; exit 0; }; [ -f "${FAKE_SSH_STATE:-/tmp}/recv" ] && echo alive || echo gone; exit 0 ;;
    *'kill -0 4243'*) [ "${FAKE_PROC_ALIVE:-1}" = "0" ] && { echo gone; exit 0; }; [ -f "${FAKE_SSH_STATE:-/tmp}/disp" ] && echo alive || echo gone; exit 0 ;;
    *'kill -0'*) [ "${FAKE_PROC_ALIVE:-1}" = "1" ] && echo alive || echo gone; exit 0 ;;
    *'kill 4242'*) [ "${FAKE_RECV_ALIVE:-0}" = "1" ] || rm -f "${FAKE_SSH_STATE:-/tmp}/recv"; exit 0 ;;
    *'kill 4243'*) [ "${FAKE_DISP_ALIVE:-0}" = "1" ] || rm -f "${FAKE_SSH_STATE:-/tmp}/disp"; exit 0 ;;
    *nohup*--test-only-autostart*)
        case "$cmd" in
            *--test-only-control-stdin*)
                mkdir -p "${FAKE_SSH_STATE:-/tmp}"; touch "${FAKE_SSH_STATE:-/tmp}/disp"
                # A new display process owns a fresh painted-frame counter.
                rm -f "$state/frames_n"
                echo 4243; exit 0 ;;
            *) echo "fake-ssh: display started without --test-only-control-stdin" >&2; exit 97 ;;
        esac ;;
    *nohup*)
        mkdir -p "${FAKE_SSH_STATE:-/tmp}"; touch "${FAKE_SSH_STATE:-/tmp}/recv"
        echo 4242; exit 0 ;;
    *'"event":"status"'*)
        probe="$(cat "$state/status_n" 2>/dev/null || echo 0)"
        probe=$((probe+1)); printf '%s' "$probe" > "$state/status_n"
        if [ "$probe" = "${FAKE_BASELINE_PROBE:-1}" ] && [ "${FAKE_BASELINE_STATUS+x}" = x ]; then
            printf '%s' "$FAKE_BASELINE_STATUS"
            exit "${FAKE_BASELINE_RC:-0}"
        fi
        if [ "$probe" -gt 1 ] && [ "${FAKE_POST_STATUS+x}" = x ]; then
            printf '%s' "$FAKE_POST_STATUS"; exit "${FAKE_POST_RC:-0}"
        fi
        if [ -n "${FAKE_DISPLAY_STATUS:-}" ]; then printf '%s\n' "$FAKE_DISPLAY_STATUS" >> "$state/frames_trace"; printf '%s' "${FAKE_DISPLAY_STATUS}"; exit 0; fi
        n="$(cat "${FAKE_SSH_STATE:-/tmp}/frames_n" 2>/dev/null || echo 0)"
        n=$((n+1)); mkdir -p "${FAKE_SSH_STATE:-/tmp}"; echo "$n" > "${FAKE_SSH_STATE:-/tmp}/frames_n"
        value="{\"event\":\"status\",\"frames_painted\":$(( ${FAKE_FRAMES_BASE:-3} + ${FAKE_FRAMES_STEP:-2} * (n-1) )),\"render_ok\":true}"
        printf '%s\n' "$value" >> "$state/frames_trace"
        printf '%s' "$value"
        exit 0 ;;
    *'"event":"active"'*) [ "${FAKE_ACTIVE:-1}" = "1" ] && exit 0 || exit 1 ;;
    *unbind*) exit "${FAKE_UNBIND_RC:-0}" ;;
    *bind*) exit "${FAKE_BIND_RC:-0}" ;;
    *'printf'*) exit 0 ;;
    *) exit 0 ;;
esac
"""

CLOCK_SHIM = r"""#!/usr/bin/env python3
import os
import pathlib
import sys
if sys.argv[1:] != ['-c', 'import time; print(time.monotonic_ns())']:
    os.execv(os.environ['REAL_PYTHON'], [os.environ['REAL_PYTHON'], *sys.argv[1:]])
state = pathlib.Path(os.environ['FAKE_SSH_STATE'])
state.mkdir(exist_ok=True)
clock = state / 'clock'
now = int(clock.read_text()) if clock.exists() else 1000000000
countdown = state / 'terminal_countdown'
if countdown.exists():
    count = int(countdown.read_text()) - 1
    countdown.write_text(str(count))
    if count <= 0:
        now = 1000000000 + int(os.environ['FAKE_TERMINAL_MS']) * 1000000
        countdown.unlink()
clock.write_text(str(now))
print(now)
"""

SLEEP_SHIM = r"""#!/usr/bin/env python3
import os
import pathlib
import sys
state = pathlib.Path(os.environ['FAKE_SSH_STATE'])
clock = state / 'clock'
now = int(clock.read_text()) if clock.exists() else 1000000000
if os.environ.get('FAKE_FREEZE_SLEEP') != '1':
    now += int(float(sys.argv[1]) * 1000000000)
clock.write_text(str(now))
"""


class RecoveryQaGates(unittest.TestCase):
    script = ""
    recovery_scenarios = ("receiver_restart", "consumer_restart", "usb_unplug_replug",
                          "phone_lock_unlock", "phone_aa_restart", "unknown_phone")

    def passing_env(self) -> dict[str, str]:
        return {
            "FAKE_FRAMES_BASE": "5", "FAKE_FRAMES_STEP": "2",
            "FAKE_MEDIA_TIMING": "1", "FAKE_INPUT_OK": "1",
            "FAKE_LOG_WINDOW": ('{"event":"diag","phone_id":"id-approved"}\n'
                                '{"kind":"reconnect","phone_id":"id-approved"}'),
            "FAKE_ATTEMPT_LOG": ('{"event":"err","detail":"link",'
                                 '"code":"transport_unauthorized_peer",'
                                 '"phone_id":"id-second","port":"1-2.3"}'),
        }

    def assert_budget_failure(self, code: int, doc: dict) -> None:
        self.assertNotEqual(code, 0, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["scenarios"][0]["status"], "FAILED")
        self.assertEqual(doc["scenarios"][0]["detail"], "recovery_exceeded_budget")

    def test_contract_budget_cannot_be_enlarged(self):
        for budget in ("0", "5001", "8000", "18446744073709551617"):
            for cli in (False, True):
                with self.subTest(budget=budget, cli=cli):
                    args = ["--readiness-only"] + (["--budget-ms", budget] if cli else [])
                    code, doc = self.run_qa(args, **({} if cli else {"BUDGET_MS": budget}))
                    self.assertEqual(code, 2, self.last_stderr)
                    self.assertEqual(doc, {})

    def test_terminal_boundary_all_scenarios(self):
        for scenario in self.recovery_scenarios:
            for boundary in (4999, 5000, 5001):
                with self.subTest(scenario=scenario, boundary=boundary):
                    env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_FREEZE_SLEEP": "1",
                                                "FAKE_TERMINAL_MS": str(boundary)}
                    code, doc = self.run_qa(["--only", scenario], **env)
                    row = doc["scenarios"][0]
                    self.assertEqual(row["recovery_ms"], boundary, doc)
                    if boundary < 5000:
                        self.assertEqual(code, 0, doc)
                        self.assertEqual(row["status"], "PASS")
                    else:
                        self.assert_budget_failure(code, doc)

    def test_first_stimulus_origin_all_scenarios(self):
        stimulus_tokens = ("kill 4242", "kill 4243", "drivers/usb/unbind",
                           "keyevent 26", "am force-stop", "--no-bus --duration-ms")
        for scenario, token in zip(self.recovery_scenarios, stimulus_tokens, strict=True):
            with self.subTest(scenario=scenario):
                env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_FIRST_DELAY": "5"}
                code, doc = self.run_qa(["--only", scenario], **env)
                self.assertIn(token, self.last_calls)
                self.assert_budget_failure(code, doc)
                self.assertIsNone(doc["scenarios"][0]["recovery_ms"])
                self.assertGreaterEqual(doc["scenarios"][0]["elapsed_ms"], 5000)
                if scenario == "usb_unplug_replug":
                    self.assertIn("drivers/usb/bind", self.last_calls)

    def test_real_late_media_reproduction(self):
        env = self.passing_env() | {"FAKE_MEDIA_DELAY": "5.2"}
        code, doc = self.run_qa(["--only", "consumer_restart"], **env)
        self.assert_budget_failure(code, doc)
        self.assertIsNone(doc["scenarios"][0]["recovery_ms"])

    def test_real_late_admission_reproduction(self):
        env = self.passing_env() | {"FAKE_ATTEMPT_DELAY": "5.2"}
        code, doc = self.run_qa(["--only", "unknown_phone"], **env)
        self.assert_budget_failure(code, doc)
        self.assertIsNone(doc["scenarios"][0]["recovery_ms"])

    def test_late_rejection_log_retrieval(self):
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_ATTEMPT_LOG_DELAY": "5"}
        code, doc = self.run_qa(["--only", "unknown_phone"], **env)
        self.assertTrue(any(line.startswith("3|") and "--no-bus" in line for line in self.last_outcomes))
        self.assertIn("tail -c +1 /tmp/aa-wired-receiver-attempt.log", self.last_calls)
        self.assert_budget_failure(code, doc)

    def test_cumulative_probe_time(self):
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_COMMAND_DELAY": "0.8",
                                    "EVIDENCE_WAIT_MS": "8000"}
        code, doc = self.run_qa(["--only", "consumer_restart"], **env)
        self.assert_budget_failure(code, doc)
        self.assertGreater(len(self.last_outcomes), 5)
        self.assertIsNone(doc["scenarios"][0]["recovery_ms"])

    def test_oversized_evidence_window_cannot_extend_deadline(self):
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_MEDIA_DELAY": "4.1",
                                    "EVIDENCE_WAIT_MS": "8000"}
        code, doc = self.run_qa(["--only", "consumer_restart"], **env)
        self.assert_budget_failure(code, doc)

    def test_finalizer_rejects_invalid_timing(self):
        cases = {
            "missing_terminal": ":",
            "unarmed": 'STIMULUS_NS=""; TERMINAL_NS=1000000100; TERMINAL_SCENARIO=consumer_restart',
            "wrong_scenario": 'TERMINAL_NS=1000000100; TERMINAL_SCENARIO=unknown_phone',
            "invalid_order": 'TERMINAL_NS=999999999; TERMINAL_SCENARIO=consumer_restart',
            "malformed_terminal": 'TERMINAL_NS=broken; TERMINAL_SCENARIO=consumer_restart',
            "malformed_origin": 'STIMULUS_NS=broken',
            "malformed_deadline": 'DEADLINE_NS=broken',
            "extended_deadline": 'DEADLINE_NS=9000000000',
            "clock_failure": 'monotonic_ns() { return 1; }',
        }
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name, mutation in cases.items():
                with self.subTest(case=name):
                    command = ('set -euo pipefail; set --; source "$SCRIPT"; '
                               'RESULTS_TSV="$PRIVATE_RESULTS"; TIMING_FAULT="$PRIVATE_FAULT"; '
                               'reset_scenario consumer_restart; STIMULUS_NS=1000000000; '
                               'DEADLINE_NS=6000000000; ' + mutation + '; '
                               'record consumer_restart PASS "" "" full candidate; '
                               'write_result FAILED test')
                    env = dict(os.environ, SCRIPT=str(self.script), OUT=str(root / "result.json"),
                               PRIVATE_RESULTS=str(root / "results"), PRIVATE_FAULT=str(root / "fault"))
                    command = command.replace('RESULTS_TSV=', 'OUT="$PRIVATE_OUT"; RESULTS_TSV=', 1)
                    env["PRIVATE_OUT"] = str(root / "result.json")
                    result = subprocess.run(["bash", "-c", command], capture_output=True,
                                            text=True, env=env, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    row = json.loads((root / "result.json").read_text())["scenarios"][-1]
                    self.assertEqual(row["status"], "FAILED")
                    self.assertTrue(row["detail"].startswith("timing_error"), row)

    def test_partial_evidence_expiry_has_no_recovery_time(self):
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_INPUT_OK": "0",
                                    "EVIDENCE_WAIT_MS": "8000"}
        code, doc = self.run_qa(["--only", "consumer_restart"], **env)
        self.assert_budget_failure(code, doc)
        row = doc["scenarios"][0]
        self.assertIsNone(row["recovery_ms"])
        self.assertGreaterEqual(row["elapsed_ms"], 5000)
        self.assertIn('"event":"input_forwarded","ok":"true"', self.last_calls)

    def test_pre_stimulus_blocking_has_no_clock_claim(self):
        env = self.passing_env() | {"FAKE_ADB_LONG": "ONLY device usb:1-2.1"}
        code, doc = self.run_qa(["--only", "unknown_phone"], **env)
        self.assertEqual(code, 3, doc)
        row = doc["scenarios"][0]
        self.assertEqual(row["status"], "BLOCKED")
        self.assertIsNone(row["recovery_ms"])
        self.assertIsNone(row["elapsed_ms"])
        self.assertNotIn("--no-bus --duration-ms", self.last_calls)

    def test_reporting_and_cleanup_do_not_change_acceptance(self):
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_FREEZE_SLEEP": "1",
                                    "FAKE_TERMINAL_MS": "1000", "FAKE_CLEANUP_DELAY": "3"}
        env.pop("FAKE_FREEZE_SLEEP")
        code, doc = self.run_qa(["--only", "unknown_phone"], **env)
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["scenarios"][0]["recovery_ms"], 1000)
        self.assertGreaterEqual(doc["wall_ms"], 7000)
        self.assertGreater(self.last_wall_ms, 0)

    def run_qa(self, args: list[str], **env: str) -> tuple[int, dict]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shim = root / "ssh"
            shim.write_text(FAKE_SSH, encoding="utf-8")
            shim.chmod(0o755)
            if env.get("FAKE_CLOCK") == "1":
                # PATH, not a production timestamp override, owns the test clock.
                for name, text in (("python3", CLOCK_SHIM), ("sleep", SLEEP_SHIM)):
                    path = root / name
                    path.write_text(text.replace("#!/usr/bin/env python3", f"#!{sys.executable}", 1), encoding="utf-8")
                    path.chmod(0o755)
            out = root / "result.json"
            command = [str(self.script), "--out", str(out)]
            if "--readiness-only" not in args:
                command += ["--receiver", "/opt/aa/recv", "--display", "/opt/aa/disp"]
            command += args
            run_env = dict(os.environ)
            run_env["PATH"] = f"{directory}:{run_env['PATH']}"
            run_env["EVIDENCE_WAIT_MS"] = "300"
            run_env["FAKE_SSH_STATE"] = str(root / "state")
            run_env["FAKE_SSH_LOG"] = str(root / "ssh.log")
            run_env["FAKE_OUTCOMES"] = str(root / "outcomes.log")
            run_env["REAL_PYTHON"] = sys.executable
            run_env.update(env)
            started = time.monotonic_ns()
            result = subprocess.run(command, capture_output=True, text=True,
                                     timeout=TIMEOUT_S, env=run_env)
            self.last_wall_ms = (time.monotonic_ns() - started) / 1_000_000
            document = json.loads(out.read_text(encoding="utf-8")) if out.exists() else {}
            self.last_stderr = result.stderr
            self.last_calls = Path(run_env["FAKE_SSH_LOG"]).read_text() if Path(run_env["FAKE_SSH_LOG"]).exists() else ""
            outcomes = root / "outcomes.log"
            self.last_outcomes = outcomes.read_text().splitlines() if outcomes.exists() else []
            frames = root / "state" / "frames_trace"
            self.last_frames = [json.loads(line)["frames_painted"] for line in frames.read_text().splitlines()] if frames.exists() else []
            return result.returncode, document

    # ---- P2-C: exact DRM status matching ----
    def test_display_connected_passes_readiness(self):
        code, doc = self.run_qa(["--readiness-only"], FAKE_DISPLAY="connected")
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "READY")

    def test_display_disconnected_fails_readiness(self):
        code, doc = self.run_qa(["--readiness-only"], FAKE_DISPLAY="disconnected")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        self.assertEqual(doc["blocked_on"], "display_disconnected")

    def test_display_card_prefixed_disconnected_fails(self):
        code, doc = self.run_qa(["--readiness-only"], FAKE_DISPLAY="card0 disconnected")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["blocked_on"], "display_disconnected")

    # ---- P2: observed evidence per scenario (or BLOCKED) ----
    def test_unknown_phone_without_rejection_is_blocked(self):
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW="")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["unknown_phone"]["evidence"], "rejection_unobserved")

    def test_unknown_phone_with_observed_rejection_passes(self):
        attributed = ('{"event":"err","detail":"link","code":"transport_unauthorized_peer",'
                      '"phone_id":"id-abc123","port":"1-2.3"}')
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=attributed,
                                FAKE_ATTEMPT_LOG=attributed)
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "PASS")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("rejection", rows["unknown_phone"]["evidence"])

    def test_numeric_302_rejection_without_attribution_is_blocked(self):
        window = '{"event":"diag","code":302,"detail":"readmit_refused"}'
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=window,
                                FAKE_ATTEMPT_LOG=window)
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["unknown_phone"]["evidence"], "rejection_unattributed")

    def test_quoted_302_shape_is_not_a_rejection_signal(self):
        window = '{"event":"diag","code":"302","detail":"readmit_refused"}'
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=window,
                                FAKE_ATTEMPT_LOG=window)
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["unknown_phone"]["evidence"], "rejection_unobserved")

    def test_unknown_phone_absent_is_blocked(self):
        alone = "FAKEDEV1 device usb:1-2.1 product:fake model:Fake device:fake"
        code, doc = self.run_qa(["--only", "unknown_phone"], FAKE_ADB_LONG=alone)
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        self.assertIn("second_device_absent", doc["blocked_on"])

    # ---- item 2: the unknown-phone case must prove the SECOND device's OWN
    #      attempt is rejected, attributed by identity+port (never shim-only) ----
    def test_unknown_phone_rejection_requires_attribution(self):
        bare = '{"event":"err","detail":"link","code":"transport_unauthorized_peer"}'
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=bare,
                                FAKE_ATTEMPT_LOG=bare)
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["unknown_phone"]["evidence"], "rejection_unattributed")

    def test_unknown_phone_rejection_on_approved_port_is_failed(self):
        attributed = ('{"event":"err","detail":"link","code":"transport_unauthorized_peer",'
                      '"phone_id":"id-abc123","port":"1-2.1"}')
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=attributed,
                                FAKE_ATTEMPT_LOG=attributed)
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["unknown_phone"]["detail"],
                         "rejection_attributed_to_approved_phone")

    def test_unknown_phone_drives_second_device_through_admission(self):
        attributed = ('{"event":"err","detail":"link","code":"transport_unauthorized_peer",'
                      '"phone_id":"id-abc123","port":"1-2.3"}')
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(["--only", "unknown_phone"],
                                    FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=attributed,
                                    FAKE_ATTEMPT_LOG=attributed, FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 0, doc)
            logged = log.read_text(encoding="utf-8")
            attempts = [line for line in logged.splitlines()
                        if "aa-wired-receiver-attempt.log" in line and "--port" in line]
            self.assertTrue(attempts, logged)
            self.assertIn("--port 1-2.3", attempts[0])
            self.assertNotIn("--port 1-2.1", attempts[0])

    def test_unknown_phone_requires_identity_port_differ_from_approved(self):
        same_serial = ("FAKEAPPROVED device usb:1-2.1 product:fake model:Fake device:fake\n"
                       "FAKEAPPROVED device usb:1-2.3 product:fake model:Fake device:fake")
        attributed = ('{"event":"err","detail":"link","code":"transport_unauthorized_peer",'
                      '"phone_id":"id-abc123","port":"1-2.3"}')
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_LONG=same_serial,
                                FAKE_LOG_WINDOW=attributed, FAKE_ATTEMPT_LOG=attributed)
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        self.assertIn("second_device_absent", doc["blocked_on"])

    def test_unknown_phone_activation_despite_rejection_is_failed(self):
        attributed = ('{"event":"err","detail":"link","code":"transport_unauthorized_peer",'
                      '"phone_id":"id-abc123","port":"1-2.3"}')
        window = attributed + '\n{"event":"active","startup_ms":100}'
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=attributed,
                                FAKE_ATTEMPT_LOG=window)
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["unknown_phone"]["detail"], "unknown_device_reached_active")

    def test_recovery_without_observed_dip_is_blocked(self):
        code, doc = self.run_qa(["--only", "phone_lock_unlock"], FAKE_LOG_WINDOW="")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["phone_lock_unlock"]["evidence"], "dip_unobserved")

    # ---- item 3: phone stimuli are fail-closed and the dip must correlate to
    #      the intended device/event via the same identity attribution ----
    def test_phone_stimulus_failure_is_fail_closed(self):
        window = ('{"event":"diag","kind":"session_state_changed","phone_id":"id-approved"}\n'
                  '{"kind":"reconnect","reason":"link lost","status":"pending","phone_id":"id-approved"}')
        code, doc = self.run_qa(["--only", "phone_lock_unlock"],
                                FAKE_ADB_STIMULUS_RC="1", FAKE_LOG_WINDOW=window)
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["phone_lock_unlock"]["detail"], "stimulus_failed")

    def test_phone_stimulus_targets_port_matched_device(self):
        window = ('{"event":"diag","kind":"session_state_changed","phone_id":"id-approved"}\n'
                  '{"kind":"reconnect","reason":"link lost","status":"pending","phone_id":"id-approved"}')
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(["--only", "phone_lock_unlock"],
                                    FAKE_LOG_WINDOW=window,
                                    FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1",
                                    FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 0, doc)
            logged = log.read_text(encoding="utf-8")
            stimuli = [line for line in logged.splitlines() if "keyevent 26" in line]
            self.assertTrue(stimuli, logged)
            self.assertIn("adb -s FAKEAPPROVED", stimuli[0])

    def test_phone_dip_must_correlate_to_intended_device(self):
        window = ('{"event":"diag","kind":"session_state_changed","phone_id":"id-approved"}\n'
                  '{"kind":"reconnect","reason":"link lost","status":"pending","phone_id":"id-OTHER"}')
        code, doc = self.run_qa(["--only", "phone_lock_unlock"], FAKE_LOG_WINDOW=window)
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["phone_lock_unlock"]["detail"],
                         "dip_not_attributed_to_intended_device")

    # ---- item 4: receiver/display start+validate and an initial projecting
    #      state BEFORE any scenario, including --only runs ----
    def test_only_run_starts_stack_before_scenarios(self):
        window = ('{"event":"diag","kind":"session_state_changed","phone_id":"id-approved"}\n'
                  '{"kind":"reconnect","reason":"link lost","status":"pending","phone_id":"id-approved"}')
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(["--only", "phone_lock_unlock"],
                                    FAKE_LOG_WINDOW=window,
                                    FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1",
                                    FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 0, doc)
            lines = log.read_text(encoding="utf-8").splitlines()
            recv_start = next(i for i, line in enumerate(lines)
                              if "nohup" in line and "aa-wired-receiver.log" in line)
            disp_start = next(i for i, line in enumerate(lines)
                              if "--test-only-autostart" in line)
            stimulus = next(i for i, line in enumerate(lines) if "keyevent 26" in line)
            self.assertLess(recv_start, stimulus, lines)
            self.assertLess(disp_start, stimulus, lines)

    def test_setup_requires_initial_projection(self):
        code, doc = self.run_qa(["--only", "phone_lock_unlock"],
                                FAKE_STATE='string "stopped"', BUDGET_MS="300")
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["status"], "FAILED")
        names = [row["name"] for row in doc["scenarios"]]
        self.assertNotIn("phone_lock_unlock", names)
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["setup"]["detail"], "no_initial_projection")

    def test_setup_validates_processes(self):
        code, doc = self.run_qa(["--only", "phone_lock_unlock"],
                                FAKE_PROC_ALIVE="0", BUDGET_MS="300")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["setup"]["detail"], "receiver_not_running")

    # ---- item 5: process-death probes track the child PIDs and verify
    #      termination BY PID - the shim's pgrep -f self-matches by design ----
    def test_process_death_verified_by_pid_not_pgrep(self):
        code, doc = self.run_qa(["--only", "receiver_restart"],
                                FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertEqual(code, 0, doc)
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["receiver_restart"]["status"], "PASS")
        self.assertEqual(rows["receiver_restart"]["detail"], "recovered")

    def test_death_probe_kills_the_tracked_child_pid(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(["--only", "consumer_restart"],
                                    FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1",
                                    FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 0, doc)
            logged = log.read_text(encoding="utf-8")
            self.assertIn("kill 4243", logged)
            self.assertNotIn("pkill", logged)
            self.assertNotIn("pgrep", logged)

    # ---- item 6: the cleanup rebinds a port left unbound - the device is
    #      never left unbound after the run ----
    def test_cleanup_rebinds_after_unbind_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(["--only", "usb_unplug_replug"],
                                    FAKE_BIND_RC="1", FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 1, doc)
            self.assertEqual(doc["status"], "FAILED")
            rows = {row["name"]: row for row in doc["scenarios"]}
            self.assertEqual(rows["usb_unplug_replug"]["detail"],
                             "sysfs_bind_requires_passwordless_sudo")
            logged = log.read_text(encoding="utf-8")
            binds = [line for line in logged.splitlines() if "drivers/usb/bind" in line]
            self.assertGreaterEqual(len(binds), 2, logged)

    # ---- item 7: recovery_ms is ENFORCED against the budget on PASS and
    #      retries are the per-disturbance delta, not the cumulative maximum ----
    def test_recovery_budget_miss_is_failed(self):
        code, doc = self.run_qa(["--only", "consumer_restart"],
                                FAKE_POST_STATE_DELAY="1", BUDGET_MS="1500",
                                FAKE_FRAMES_BASE="5", FAKE_FRAMES_STEP="2",
                                FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["consumer_restart"]["detail"], "recovery_exceeded_budget")
        self.assertIsNone(rows["consumer_restart"]["recovery_ms"])
        self.assertGreaterEqual(rows["consumer_restart"]["elapsed_ms"], 1500)

    def test_retry_count_is_per_disturbance_delta(self):
        code, doc = self.run_qa(["--only", "consumer_restart"],
                                FAKE_RETRIES_BASE="2", FAKE_RETRIES_STEP="3",
                                FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertEqual(code, 0, doc)
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["consumer_restart"]["retries"], 3)

    # ---- item 8: painted frames must GROW past the recorded baseline - a
    #      stuck counter is not post-recovery painting ----
    def test_frames_painted_requires_post_recovery_growth(self):
        stuck = '{"event":"status","frames_painted":7,"render_ok":true}'
        code, doc = self.run_qa(["--only", "consumer_restart"],
                                FAKE_DISPLAY_STATUS=stuck,
                                FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertNotEqual(code, 0, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("media_unobserved", rows["consumer_restart"]["evidence"])

    def test_frames_painted_baseline_is_recorded(self):
        code, doc = self.run_qa(["--only", "consumer_restart"],
                                FAKE_FRAMES_BASE="5", FAKE_FRAMES_STEP="2",
                                FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertEqual(code, 0, doc)
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["consumer_restart"]["frames_painted_baseline"], 5)

    def test_baseline_failure_stops_all_scenarios_before_disturbance(self) -> None:
        # Given: only the baseline fails; later status is stale 7, other media is good.
        stale = '{"event":"status","frames_painted":7,"render_ok":true}'
        cases = (("command_failure", "", "1"), ("failed_with_output", stale, "1"),
                 ("empty", "", "0"), ("malformed_json", "broken", "0"),
                 ("missing", '{"event":"status","render_ok":true}', "0"),
                 ("malformed", '{"frames_painted":7.5,"render_ok":true}', "0"),
                 ("negative", '{"frames_painted":-1,"render_ok":true}', "0"))
        for scenario in self.recovery_scenarios[:-1]:
            for name, status, rc in cases:
                with self.subTest(scenario=scenario, case=name):
                    env = self.passing_env() | {
                        "FAKE_CLOCK": "1", "FAKE_BASELINE_STATUS": status,
                        "FAKE_BASELINE_RC": rc, "FAKE_DISPLAY_STATUS": stale}
                    # When: invoke the real set-e harness, including its receipt finalizer.
                    code, doc = self.run_qa(["--only", scenario], **env)
                    # Then: one honest failed receipt, without a scenario disturbance.
                    self.assertEqual(code, 1, doc)
                    self.assertEqual(doc["status"], "FAILED")
                    self.assertEqual(len(doc["scenarios"]), 1)
                    row = doc["scenarios"][0]
                    self.assertEqual(row["detail"], "frames_baseline_unavailable")
                    self.assertIsNone(row["frames_painted_baseline"])
                    self.assertIsNone(row["elapsed_ms"])
                    calls = self.last_calls.splitlines()
                    # Cleanup kills are allowed, but only AFTER the single status probe.
                    probe = next(i for i, call in enumerate(calls) if '\"event\":\"status\"' in call)
                    self.assertFalse(any("kill 424" in call or "drivers/usb/unbind" in call
                                         or "keyevent 26" in call or "am force-stop" in call
                                         for call in calls[:probe]))
                    self.assertEqual(sum('\"event\":\"status\"' in call for call in calls), 1)
                    self.assertNotIn("drivers/usb/unbind", self.last_calls)
                    self.assertNotIn("keyevent 26", self.last_calls)
                    self.assertNotIn("am force-stop", self.last_calls)

    def test_baseline_zero_with_growth_passes_all_scenarios(self) -> None:
        for scenario in self.recovery_scenarios[:-1]:
            with self.subTest(scenario=scenario):
                # Given: a real zero baseline and positive post-recovery painting.
                env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_FRAMES_BASE": "0"}
                # When: drive each baseline caller through the CLI.
                code, doc = self.run_qa(["--only", scenario], **env)
                # Then: zero remains valid evidence, not an unavailable sentinel.
                self.assertEqual(code, 0, doc)
                self.assertEqual(doc["scenarios"][0]["frames_painted_baseline"], 0)
                self.assertEqual(doc["scenarios"][0]["status"], "PASS")

    def test_baseline_shim_failure_leaves_later_stale_status_available(self) -> None:
        # Given: a single failed baseline command, independent later stale 7.
        stale = '{"event":"status","frames_painted":7,"render_ok":true}'
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, FAKE_SSH_STATE=directory,
                       FAKE_OUTCOMES=f"{directory}/outcomes", FAKE_BASELINE_STATUS="",
                       FAKE_BASELINE_RC="1", FAKE_DISPLAY_STATUS=stale)
            command = ('export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"; '
                       'export DISPLAY=:1; grep \'"event":"status"\'')
            # When: run the same fake SSH status command twice.
            first = subprocess.run(["bash", "-c", FAKE_SSH, "ssh", command],
                                   capture_output=True, text=True, env=env, timeout=5)
            later = subprocess.run(["bash", "-c", FAKE_SSH, "ssh", command],
                                   capture_output=True, text=True, env=env, timeout=5)
            # Then: only baseline fails; a later probe is valid and unchanged at 7.
            self.assertEqual((first.returncode, first.stdout), (1, ""))
            self.assertEqual((later.returncode, later.stdout), (0, stale))

    def test_baseline_growth_helper_rejects_invalid_baseline_before_probe(self) -> None:
        for baseline in ("", "-1", "broken", "7.5", "07", "1+1"):
            with self.subTest(baseline=baseline):
                # Given: a valid-looking later frame, but no valid baseline.
                command = ('set -euo pipefail; set --; source "$SCRIPT"; '
                           'remote() { printf unexpected_probe >&2; '
                           'printf \'{"frames_painted":7,"render_ok":true}\'; }; '
                           'if painted_frames_grew "$BASELINE"; then exit 9; fi')
                env = dict(os.environ, SCRIPT=str(self.script), BASELINE=baseline)
                # When: use the shared helper under parent-shell errexit.
                result = subprocess.run(["bash", "-c", command], capture_output=True,
                                        text=True, env=env, timeout=5)
                # Then: invalid arithmetic operands are rejected normally, never accepted.
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stderr, "")

    def test_baseline_failure_after_consumer_reset_writes_receipt(self) -> None:
        # Given: pre-disturbance status works but the new consumer has no baseline.
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_BASELINE_PROBE": "2",
                                    "FAKE_BASELINE_STATUS": "", "FAKE_BASELINE_RC": "1"}
        # When: restart the consumer.
        code, doc = self.run_qa(["--only", "consumer_restart"], **env)
        # Then: caught command substitution failure still reaches JSON finalization.
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["scenarios"][0]["detail"], "frames_baseline_unavailable")
        self.assertIsNone(doc["scenarios"][0]["frames_painted_baseline"])

    def test_baseline_valid_does_not_authorize_invalid_post_status(self) -> None:
        cases = ('{"frames_painted":7.5,"render_ok":true}',
                 '{"frames_painted":7e2,"render_ok":true}',
                 '{"frames_painted":-7,"render_ok":true}',
                 '{"frames_painted":7oops,"render_ok":true}',
                 '{"frames_painted":true,"render_ok":true}',
                 '{"frames_painted":7,"render_ok":false}',
                 '{"frames_painted":7,"render_ok":"true"}',
                 '{"frames_painted":5,"render_ok":true}')
        for status in cases:
            with self.subTest(status=status):
                # Given: valid 5 baseline and bad/stale post status, audio/input good.
                env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_POST_STATUS": status}
                # When: recover without restarting the display counter.
                code, doc = self.run_qa(["--only", "receiver_restart"], **env)
                # Then: malformed/negative/stale counters or missing rendering never pass.
                self.assertEqual(code, 1, doc)
                self.assertEqual(doc["scenarios"][0]["status"], "FAILED")

    def test_baseline_valid_does_not_authorize_failed_post_command(self) -> None:
        # Given: valid-looking growth from a command that failed.
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_POST_RC": "1",
                                    "FAKE_POST_STATUS": '{"frames_painted":7,"render_ok":true}'}
        # When: the media probe runs after recovery.
        code, doc = self.run_qa(["--only", "receiver_restart"], **env)
        # Then: command success is required as well as payload evidence.
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["scenarios"][0]["status"], "FAILED")

    def test_consumer_restart_requires_process_death(self):
        code, doc = self.run_qa(["--only", "consumer_restart"], FAKE_DISP_ALIVE="1")
        self.assertNotEqual(code, 0, doc)
        self.assertEqual(doc["status"], "FAILED")

    # ---- P2-G: GetState=projecting without media is NON-PASS ----
    def test_projecting_without_media_is_not_pass(self):
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_DISPLAY_STATUS='{"event":"status","frames_painted":0,"render_ok":false}',
            FAKE_MEDIA_TIMING="0", FAKE_INPUT_OK="0")
        self.assertNotEqual(code, 0, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("media_unobserved", rows["consumer_restart"]["evidence"])
        self.assertEqual(rows["consumer_restart"]["detail"],
                         "no_media_evidence_after_recovery")

    def test_projecting_with_full_media_evidence_passes(self):
        window = '{"event":"media_timing","av_sync_ms":12,"audio_latency_ms":40}'
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1",
            FAKE_LOG_WINDOW=window)
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "PASS")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("frames+pump+input", rows["consumer_restart"]["evidence"])

    def test_painted_frames_zero_is_not_media(self):
        status = '{"event":"status","frames_painted":0,"render_ok":true}'
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_DISPLAY_STATUS=status, FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertNotEqual(code, 0, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("media_unobserved", rows["consumer_restart"]["evidence"])

    # ---- N1: every media conjunct independently gates the verdict ----
    def test_audio_pump_missing_is_not_media(self):
        for enabled in ("0", "1"):
            with self.subTest(media_timing=enabled):
                code, doc = self.run_qa(
                    ["--only", "consumer_restart"],
                    FAKE_FRAMES_BASE="5", FAKE_FRAMES_STEP="2",
                    FAKE_MEDIA_TIMING=enabled, FAKE_AUDIO_CHUNKS="1", FAKE_INPUT_OK="1")
                row = doc["scenarios"][0]
                probes = [line for line in self.last_outcomes if '\"event\":\"media_timing\"' in line]
                self.assertTrue(probes, self.last_calls)
                self.assertTrue(all(line.startswith(f"{1-int(enabled)}|") for line in probes))
                self.assertEqual(row["frames_painted_baseline"], 5)
                self.assertEqual(self.last_frames[0], 5)
                self.assertGreater(max(self.last_frames), 5)
                self.assertEqual(code, 0 if enabled == "1" else 1, doc)
                self.assertEqual(row["status"], "PASS" if enabled == "1" else "FAILED")
                if enabled == "0":
                    self.assertEqual(row["detail"], "no_media_evidence_after_recovery")
                self.assert_fixture_available("audio_chunks", "FAKE_AUDIO_CHUNKS")

    def test_final_flush_timing_without_delivered_audio_is_not_media(self) -> None:
        env = self.passing_env() | {"FAKE_CLOCK": "1", "FAKE_AUDIO_CHUNKS": "0"}
        code, doc = self.run_qa(["--only", "consumer_restart"], **env)
        self.assertEqual(code, 1, doc)
        self.assertEqual(doc["scenarios"][0]["detail"], "no_media_evidence_after_recovery")

    def test_input_ok_true_missing_is_not_media(self):
        for enabled in ("0", "1"):
            with self.subTest(input_ok=enabled):
                code, doc = self.run_qa(
                    ["--only", "consumer_restart"],
                    FAKE_FRAMES_BASE="5", FAKE_FRAMES_STEP="2", FAKE_MEDIA_TIMING="1",
                    FAKE_INPUT_OK=enabled, FAKE_INPUT_ANY="1")
                row = doc["scenarios"][0]
                audio = [line for line in self.last_outcomes if '\"event\":\"media_timing\"' in line]
                inputs = [line for line in self.last_outcomes if '\"event\":\"input_forwarded\",\"ok\":\"true\"' in line]
                self.assertTrue(audio, self.last_calls)
                self.assertTrue(all(line.startswith("0|") for line in audio))
                self.assertTrue(inputs, self.last_calls)
                self.assertTrue(all(line.startswith(f"{1-int(enabled)}|") for line in inputs))
                self.assertEqual(self.last_frames[0], 5)
                self.assertGreater(max(self.last_frames), 5)
                self.assertEqual(code, 0 if enabled == "1" else 1, doc)
                self.assertEqual(row["status"], "PASS" if enabled == "1" else "FAILED")
                if enabled == "0":
                    self.assertEqual(row["detail"], "no_media_evidence_after_recovery")
                self.assert_fixture_available("input_forwarded", "FAKE_INPUT_ANY")

    def assert_fixture_available(self, token: str, variable: str) -> None:
        with tempfile.TemporaryDirectory() as directory:
            env = dict(os.environ, FAKE_SSH_STATE=directory,
                       FAKE_OUTCOMES=f"{directory}/outcomes", **{variable: "1"})
            command = ('export DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u)/bus"; '
                       f"export DISPLAY=:1; grep -q '{token}'")
            result = subprocess.run(["bash", "-c", FAKE_SSH, "ssh", command],
                                    capture_output=True, env=env, timeout=5)
            self.assertEqual(result.returncode, 0, result.stderr)

    # ---- P3: subset recording, hostile args, redaction, budget labelling ----
    def test_subset_run_records_only_field_and_verdict(self):
        code, doc = self.run_qa(["--only", "usb_unplug_replug"],
                                FAKE_LOG_WINDOW='{"kind":"reconnect"}',
                                FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["only"], "usb_unplug_replug")
        self.assertTrue(doc["subset"])
        self.assertIn("SUBSET", doc["note"])
        self.assertIn("NOT a full-matrix pass", doc["note"])

    def test_full_matrix_run_records_null_only(self):
        code, doc = self.run_qa([], FAKE_LOG_WINDOW='{"kind":"reconnect"}',
                                FAKE_ADB_COUNT="1")
        self.assertIn(doc["only"], (None, "null"))
        self.assertFalse(doc["subset"])
        self.assertNotIn("SUBSET", doc["note"])

    # ---- N3: --only must fail closed on empty/unknown/malformed selection ----
    def assert_only_usage_error(self, args):
        code, doc = self.run_qa(args)
        self.assertEqual(code, 2, doc)
        self.assertEqual(doc, {})
        self.assertIn("accepted values", self.last_stderr)
        self.assertIn("usb_unplug_replug", self.last_stderr)
        self.assertIn("malformed_input", self.last_stderr)

    def test_only_unknown_name_is_usage_error(self):
        self.assert_only_usage_error(["--only", "bogus"])

    def test_only_empty_value_is_usage_error(self):
        self.assert_only_usage_error(["--only", ""])

    def test_only_malformed_list_is_usage_error(self):
        self.assert_only_usage_error(["--only", "usb_unplug_replug "])

    def test_only_trailing_comma_is_usage_error(self):
        self.assert_only_usage_error(["--only", "usb_unplug_replug,"])

    def test_only_missing_value_is_usage_error(self):
        self.assert_only_usage_error(["--only"])

    def test_hostile_host_leading_dash_is_refused(self):
        code, doc = self.run_qa(["--readiness-only", "--host", "-oProxyCommand=touch /tmp/pwned"])
        self.assertEqual(code, 2)
        self.assertEqual(doc, {})

    def test_remote_args_are_shell_escaped(self):
        raw = "1-2.1;echo INJECTED"
        escaped = r"1-2.1\;echo\ INJECTED"
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(["--readiness-only", "--port", raw], FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 0, doc)
            logged = log.read_text(encoding="utf-8")
            self.assertIn(escaped, logged)
            self.assertNotIn(raw, logged)

    def test_host_never_written_raw_to_artifact(self):
        sentinel = "sentinel-host-xyz"
        code, doc = self.run_qa(["--readiness-only", "--host", sentinel])
        self.assertEqual(code, 0, doc)
        dumped = json.dumps(doc)
        self.assertNotIn(sentinel, dumped)
        self.assertTrue(doc.get("host"), doc)
        self.assertTrue(str(doc["host"]).startswith("sha256:"), doc["host"])

    def test_budget_claim_is_labelled_enforced(self):
        code, doc = self.run_qa(["--readiness-only"])
        self.assertEqual(code, 0, doc)
        self.assertIn("ENFORCED", doc["budget_note"])
        self.assertIn("5000", doc["budget_note"])
        self.assertIn("FAILED", doc["budget_note"])

    def test_display_started_with_real_test_control_flag(self):
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertEqual(code, 0, doc)

    def test_receiver_restart_media_window_covers_fresh_log(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(
                ["--only", "receiver_restart"],
                FAKE_STATE='string "projecting"', FAKE_RECV_ALIVE="0",
                FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1",
                FAKE_LOG_SIZE="4096", FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 0, doc)
            logged = log.read_text(encoding="utf-8")
            self.assertIn("tail -c +1 /tmp/aa-wired-receiver.log", logged)
            self.assertNotIn("tail -c +4097", logged)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: test_recovery_qa_gates.py <run-wired-recovery-qa.sh>")
    RecoveryQaGates.script = Path(sys.argv[1])
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RecoveryQaGates)
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
