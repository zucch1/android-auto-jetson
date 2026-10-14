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
import unittest
from pathlib import Path

TIMEOUT_S = 90

FAKE_SSH = r"""#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_SSH_LOG:-}" ]; then
    printf '%s\n' "$*" >> "$FAKE_SSH_LOG"
fi
cmd="${@: -1}"
all_args="$*"
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
    *'n++'*) printf '%s' "${FAKE_ADB_COUNT:-2}"; exit 0 ;;
    *'print $1'*) printf '%s\n' "${FAKE_ADB_LIST:-FAKEDEV1 FAKEDEV2}" | tr ' ' '\n'; exit 0 ;;
    *'adb shell'*) exit 0 ;;
    *'wc -c'*) printf '%s' "${FAKE_LOG_SIZE:-0}"; exit 0 ;;
    # Media-gate probes (N1): both probe commands contain their evidence token
    # AND `tail -c +`, so first-match case ordering decides which arm drives
    # them. The exact-shape arms MUST precede the raw-window tail arm (the
    # tail arm also serves dip_observed/rejection_observed window reads, which
    # carry no media tokens). The wrong-marker arms (audio_chunks / bare
    # input_forwarded) model the pre-P2-G defect shape: a gate that regressed
    # to presence greps lands there, so the per-conjunct pins can prove it.
    *'"event":"media_timing"'*) [ "${FAKE_MEDIA_TIMING:-0}" = "1" ] && exit 0 || exit 1 ;;
    *'"event":"input_forwarded","ok":"true"'*) [ "${FAKE_INPUT_OK:-0}" = "1" ] && exit 0 || exit 1 ;;
    *audio_chunks*) [ "${FAKE_AUDIO_CHUNKS:-0}" = "1" ] && exit 0 || exit 1 ;;
    *'input_forwarded'*) [ "${FAKE_INPUT_ANY:-0}" = "1" ] && exit 0 || exit 1 ;;
    *'tail -c +'*) printf '%s' "${FAKE_LOG_WINDOW:-}"; exit 0 ;;
    *'retries'*) printf '%s' "${FAKE_RETRIES:-1}"; exit 0 ;;
    *busctl*) printf '%s' "${FAKE_STATE:-string \"projecting\"}"; exit 0 ;;
    *pgrep*aa-wired-receiver*) [ "${FAKE_RECV_ALIVE:-0}" = "1" ] && echo alive || echo gone; exit 0 ;;
    *pgrep*aa-dummy-display*) [ "${FAKE_DISP_ALIVE:-0}" = "1" ] && echo alive || echo gone; exit 0 ;;
    *pkill*) exit 0 ;;
    *nohup*--test-only-autostart*)
        case "$cmd" in
            *--test-only-control-stdin*) echo 4242; exit 0 ;;
            *) echo "fake-ssh: display started without --test-only-control-stdin" >&2; exit 97 ;;
        esac ;;
    *nohup*) echo 4242; exit 0 ;;
    *'"event":"status"'*) printf '%s' "${FAKE_DISPLAY_STATUS:-{\"event\":\"status\",\"frames_painted\":0,\"render_ok\":false}}"; exit 0 ;;
    *'"event":"active"'*) [ "${FAKE_ACTIVE:-1}" = "1" ] && exit 0 || exit 1 ;;
    *unbind*) [ "${FAKE_SYSFS:-1}" = "1" ] && exit 0 || exit 1 ;;
    *bind*) [ "${FAKE_SYSFS:-1}" = "1" ] && exit 0 || exit 1 ;;
    *'printf'*) exit 0 ;;
    *) exit 0 ;;
esac
"""


class RecoveryQaGates(unittest.TestCase):
    script = ""

    def run_qa(self, args: list[str], **env: str) -> tuple[int, dict]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shim = root / "ssh"
            shim.write_text(FAKE_SSH, encoding="utf-8")
            shim.chmod(0o755)
            out = root / "result.json"
            command = [str(self.script), "--out", str(out)]
            if "--readiness-only" not in args:
                command += ["--receiver", "/opt/aa/recv", "--display", "/opt/aa/disp"]
            command += args
            run_env = dict(os.environ)
            run_env["PATH"] = f"{directory}:{run_env['PATH']}"
            run_env["EVIDENCE_WAIT_MS"] = "300"
            run_env.update(env)
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=TIMEOUT_S, env=run_env)
            document = json.loads(out.read_text(encoding="utf-8")) if out.exists() else {}
            self.last_stderr = result.stderr
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
        window = '{"event":"err","detail":"link","code":"transport_unauthorized_peer"}'
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=window,
                                FAKE_STATE='string "stopped"')
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "PASS")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("rejection", rows["unknown_phone"]["evidence"])

    def test_unknown_phone_with_numeric_302_rejection_passes(self):
        window = '{"event":"diag","code":302,"detail":"readmit_refused"}'
        code, doc = self.run_qa(["--only", "unknown_phone"],
                                FAKE_ADB_COUNT="2", FAKE_LOG_WINDOW=window,
                                FAKE_STATE='string "stopped"')
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "PASS")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("rejection", rows["unknown_phone"]["evidence"])

    def test_unknown_phone_absent_is_blocked(self):
        code, doc = self.run_qa(["--only", "unknown_phone"], FAKE_ADB_LIST="FAKEDEV1")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        self.assertIn("second_device_absent", doc["blocked_on"])

    def test_recovery_without_observed_dip_is_blocked(self):
        code, doc = self.run_qa(["--only", "phone_lock_unlock"], FAKE_LOG_WINDOW="")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertEqual(rows["phone_lock_unlock"]["evidence"], "dip_unobserved")

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
        status = '{"event":"status","frames_painted":7,"render_ok":true}'
        window = '{"event":"media_timing","av_sync_ms":12,"audio_latency_ms":40}'
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_DISPLAY_STATUS=status, FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1",
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
        status = '{"event":"status","frames_painted":7,"render_ok":true}'
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_DISPLAY_STATUS=status, FAKE_MEDIA_TIMING="0",
            FAKE_AUDIO_CHUNKS="1", FAKE_INPUT_OK="1")
        self.assertNotEqual(code, 0, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("media_unobserved", rows["consumer_restart"]["evidence"])

    def test_input_ok_true_missing_is_not_media(self):
        status = '{"event":"status","frames_painted":7,"render_ok":true}'
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_DISPLAY_STATUS=status, FAKE_MEDIA_TIMING="1",
            FAKE_INPUT_OK="0", FAKE_INPUT_ANY="1")
        self.assertNotEqual(code, 0, doc)
        self.assertEqual(doc["status"], "FAILED")
        rows = {row["name"]: row for row in doc["scenarios"]}
        self.assertIn("media_unobserved", rows["consumer_restart"]["evidence"])

    # ---- P3: subset recording, hostile args, redaction, budget labelling ----
    def test_subset_run_records_only_field_and_verdict(self):
        code, doc = self.run_qa(["--only", "usb_unplug_replug"],
                                FAKE_LOG_WINDOW='{"kind":"reconnect"}',
                                FAKE_DISPLAY_STATUS='{"event":"status","frames_painted":3,"render_ok":true}',
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

    def test_budget_claim_is_labelled_unmeasured(self):
        code, doc = self.run_qa(["--readiness-only"])
        self.assertEqual(code, 0, doc)
        self.assertIn("UNMEASURED", doc["budget_note"])
        self.assertIn("bounded wait", doc["budget_note"])

    def test_display_started_with_real_test_control_flag(self):
        status = '{"event":"status","frames_painted":3,"render_ok":true}'
        code, doc = self.run_qa(
            ["--only", "consumer_restart"],
            FAKE_STATE='string "projecting"', FAKE_DISP_ALIVE="0",
            FAKE_DISPLAY_STATUS=status, FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1")
        self.assertEqual(code, 0, doc)

    def test_receiver_restart_media_window_covers_fresh_log(self):
        status = '{"event":"status","frames_painted":3,"render_ok":true}'
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(
                ["--only", "receiver_restart"],
                FAKE_STATE='string "projecting"', FAKE_RECV_ALIVE="0",
                FAKE_DISPLAY_STATUS=status, FAKE_MEDIA_TIMING="1", FAKE_INPUT_OK="1",
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
