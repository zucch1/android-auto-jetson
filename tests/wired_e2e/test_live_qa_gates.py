#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wired_e2e/test_live_qa_gates.py <path-to-run-wired-live-qa.sh>
"""Script-level gate regressions for tools/wired-qa/run-wired-live-qa.sh (findings F3/F5/P2-G).

A fake `ssh` shim is placed on PATH and scripted through environment variables so
the live harness's readiness + verdict logic runs hermetically (no device). Each
case asserts the exact JSON verdict and exit code:

  F5  a DRM status of `disconnected` (or `cardN disconnected`) must fail display
      readiness; only an exact `connected` token passes.
  F3  PASS requires an ACTIVE session AND all three observables; a `projecting`
      D-Bus string without an active session, or any missing observable, is
      FAILED (the session ran) while a missing prerequisite is BLOCKED.
  T30 every remote ssh command must carry the target session environment: an
      explicit DBUS_SESSION_BUS_ADDRESS with a REMOTELY-expanded $(id -u) plus
      DISPLAY=:1; without it `busctl --user` fails and Qt picks xcb.
  P2-G the three observables are REAL media evidence, not marker presence:
      video requires painted frames (frames_painted > 0, render_ok true),
      audio requires the post-pump media_timing record (audio_chunks is
      emitted before pump()), input requires "ok":"true" (an ok:false
      input_forwarded record is a failed send). `projecting` with no
      observed media is NON-PASS.
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TIMEOUT_S = 60

FAKE_SSH = r"""#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_SSH_LOG:-}" ]; then
    printf '%s\n' "$*" >> "$FAKE_SSH_LOG"
fi
cmd="${@: -1}"
if [[ "$cmd" == *nohup* || "$cmd" == *kill* ]]; then
    if [[ "${FAKE_PROCESSES:-0}" == 1 ]]; then
        cmd="${cmd//\/tmp\//$FAKE_TARGET/}"
        cmd="${cmd//\/opt\/aa\/recv/$FAKE_TARGET/recv}"
        cmd="${cmd//\/opt\/aa\/disp/$FAKE_TARGET/disp}"
        if [[ "$cmd" == *nohup*disp* && "${FAKE_LAUNCH_FAILURE:-0}" == 1 ]]; then
            exit 55
        fi
        bash -c "$cmd"
        exit $?
    fi
fi
# T30 env guard: every harness ssh invocation must carry the target session
# environment, with $(id -u) left unexpanded for the TARGET shell to resolve.
all_args="$*"
case "$all_args" in
    *'DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus'*) ;;
    *) echo "fake-ssh: remote command missing target bus address: $all_args" >&2; exit 98 ;;
esac
case "$all_args" in
    *'DISPLAY=:1'*) ;;
    *) echo "fake-ssh: remote command missing DISPLAY=:1: $all_args" >&2; exit 98 ;;
esac
if [ "$cmd" = "true" ]; then
    [ "${FAKE_SSH_FAIL:-0}" = "1" ] && exit 255
    exit 0
fi
if [[ "$cmd" == *grep* ]]; then
    counter="$FAKE_LOG_ROOT/polls"
    count=0
    [[ -f "$counter" ]] && count="$(cat "$counter")"
    printf '%s' "$((count + 1))" > "$counter"
    log="$FAKE_LOG_ROOT/receiver.log"
    : > "$log"
    if ((count >= ${FAKE_DELAY_POLLS:-0})); then
        [[ "${FAKE_ACTIVE:-1}" == 1 ]] && printf '%s\n' '{"event":"active"}' >> "$log"
        [[ "${FAKE_OBS_VIDEO:-1}" == 1 ]] && printf '%s\n' '{"event":"video_sent"}' >> "$log"
        [[ "${FAKE_OBS_AUDIO:-1}" == 1 ]] && printf '%s\n' '{"event":"audio_chunks"}' >> "$log"
        if [[ "${FAKE_OBS_INPUT:-1}" == 1 ]]; then
            printf '%s\n' "{\"event\":\"input_forwarded\",\"ok\":\"${FAKE_INPUT_OK:-true}\"}" >> "$log"
        fi
    fi
    bash -c "${cmd//\/tmp\/aa-wired-receiver.log/$log}"
    exit $?
fi
case "$cmd" in
    *idVendor*) printf '%s' "${FAKE_PHONE:-04e8:6860}"; exit 0 ;;
    *"/sys/class/drm"*) printf '%s' "${FAKE_DISPLAY:-connected}"; exit 0 ;;
    *pactl*|*pw-cli*) printf '%s' "${FAKE_SINK:-sink1}"; exit 0 ;;
    *70-android-auto-receiver.rules*) printf '%s' "${FAKE_POLICY:-installed}"; exit 0 ;;
    *nohup*--test-only-autostart*)
        [[ "${FAKE_DISPLAY_LAUNCH_FAIL:-0}" == 1 ]] && exit 41
        echo 4343; exit 0 ;;
    *nohup*) echo 4242; exit 0 ;;
    *busctl*) printf '%s' "${FAKE_STATE:-string \"projecting\"}"; exit 0 ;;
    *aa-disp.in*status*)
        printf '{"event":"status","state":"projecting","frames_presented":%s,"frames_painted":%s,"helper_started":true,"input_sent":1,"input_dropped":0,"input_last_error":"","last_error":"","render_ok":%s,"render_width":1280,"render_height":720}' \
            "${FAKE_FRAMES_PAINTED:-3}" "${FAKE_FRAMES_PAINTED:-3}" "${FAKE_RENDER_OK:-true}"
        exit 0 ;;
    *aa-disp.in*) exit 0 ;;
    *active*) [ "${FAKE_ACTIVE:-1}" = "1" ] && { echo ok; exit 0; } || exit 1 ;;
    *media_timing*) [ "${FAKE_OBS_PUMP:-1}" = "1" ] && { echo ok; exit 0; } || exit 1 ;;
    *video_sent*) [ "${FAKE_OBS_VIDEO:-1}" = "1" ] && { echo ok; exit 0; } || exit 1 ;;
    *audio_chunks*) [ "${FAKE_OBS_AUDIO:-1}" = "1" ] && { echo ok; exit 0; } || exit 1 ;;
    *input_forwarded*ok*true*) [ "${FAKE_OBS_INPUT:-1}" = "1" ] && { echo ok; exit 0; } || exit 1 ;;
    *input_forwarded*) [ "${FAKE_INPUT_ANY:-1}" = "1" ] && { echo ok; exit 0; } || exit 1 ;;
    *kill*)
        bash -c 'kill() { printf "kill %s\n" "$*" >> "$FAKE_SSH_LOG"; [[ "$1" != -0 ]]; }; '"$cmd"
        exit $? ;;
    *) exit 0 ;;
esac
"""


class LiveQaGates(unittest.TestCase):
    script = ""

    def run_qa(self, readiness_only: bool, **env: str) -> tuple[int, dict]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shim = root / "ssh"
            shim.write_text(FAKE_SSH, encoding="utf-8")
            shim.chmod(0o755)
            out = root / "result.json"
            command = [str(self.script), "--out", str(out), "--projection-timeout-ms", "300"]
            if readiness_only:
                command.append("--readiness-only")
            else:
                command += ["--receiver", "/opt/aa/recv", "--display", "/opt/aa/disp"]
            run_env = dict(os.environ)
            run_env["PATH"] = f"{directory}:{run_env['PATH']}"
            run_env["FAKE_TARGET"] = directory
            run_env["FAKE_LOG_ROOT"] = directory
            run_env["FAKE_SSH_LOG"] = str(root / "ssh.log")
            run_env.update(env)
            result = subprocess.run(command, capture_output=True, text=True,
                                    timeout=TIMEOUT_S, env=run_env)
            document = json.loads(out.read_text(encoding="utf-8")) if out.exists() else {}
            return result.returncode, document

    # ---- F5: exact DRM status matching ----
    def test_display_connected_passes_readiness(self):
        code, doc = self.run_qa(True, FAKE_DISPLAY="connected")
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "READY")

    def test_display_disconnected_fails_readiness(self):
        code, doc = self.run_qa(True, FAKE_DISPLAY="disconnected")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        self.assertEqual(doc["blocked_on"], "display_disconnected")

    def test_display_card_prefixed_disconnected_fails(self):
        code, doc = self.run_qa(True, FAKE_DISPLAY="card0 disconnected")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["blocked_on"], "display_disconnected")

    # ---- F3: session activation + observable gates ----
    def test_all_observables_pass(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_AUDIO="1", FAKE_OBS_INPUT="1")
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "PASS")
        obs = doc["observables"]
        self.assertTrue(obs["projection_reached"] and obs["video_present"]
                        and obs["audio_present"] and obs["input_evidence"])

    def test_failed_input_attempt_is_not_delivery(self) -> None:
        code, doc = self.run_qa(False, FAKE_INPUT_OK="false")
        self.assertEqual(code, 4, doc)
        self.assertFalse(doc["observables"]["input_evidence"])

    def test_delayed_streams_and_activation_are_polled(self) -> None:
        code, doc = self.run_qa(False, FAKE_DELAY_POLLS="4")
        self.assertEqual(code, 0, doc)
        self.assertEqual(doc["status"], "PASS")

    def test_teardown_terminates_and_checks_both_target_pids(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(False, FAKE_SSH_LOG=str(log))
            self.assertEqual(code, 0, doc)
            logged = log.read_text(encoding="utf-8")
            for pid in (4242, 4343):
                self.assertIn(f"kill {pid}\n", logged)
                self.assertIn(f"kill -0 {pid}\n", logged)

    def test_second_launch_failure_still_cleans_receiver(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, _ = self.run_qa(False, FAKE_SSH_LOG=str(log), FAKE_DISPLAY_LAUNCH_FAIL="1")
            self.assertNotEqual(code, 0)
            self.assertIn("kill 4242\n", log.read_text(encoding="utf-8"))

    def test_projecting_without_active_session_is_failed(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="0",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_AUDIO="1", FAKE_OBS_INPUT="1")
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "session_not_active")

    def test_missing_video_is_failed(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="0", FAKE_OBS_AUDIO="1", FAKE_OBS_INPUT="1")
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "observable_missing")
        self.assertFalse(doc["observables"]["video_present"])

    def test_missing_audio_is_failed(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_PUMP="0", FAKE_OBS_INPUT="1")
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "observable_missing")
        self.assertFalse(doc["observables"]["audio_present"])

    def test_missing_input_is_failed(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_AUDIO="1", FAKE_OBS_INPUT="0")
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "observable_missing")
        self.assertFalse(doc["observables"]["input_evidence"])

    def test_target_pids_are_terminated_on_success_and_launch_failure(self) -> None:
        for failure in ("0", "1"):
            with self.subTest(launch_failure=failure), tempfile.TemporaryDirectory() as directory:
                target = Path(directory)
                for name in ("recv", "disp"):
                    program = target / name
                    program.write_text(
                        '#!/usr/bin/env bash\nset -euo pipefail\n'
                        'printf "%s\\n" "$$" > "${0}.actual-pid"\nexec sleep 60\n',
                        encoding="utf-8",
                    )
                    program.chmod(0o755)
                log = target / "commands.log"
                try:
                    code, doc = self.run_qa(
                        False, FAKE_PROCESSES="1", FAKE_TARGET=directory,
                        FAKE_LAUNCH_FAILURE=failure, FAKE_SSH_LOG=str(log),
                    )
                    self.assertEqual(code, 0 if failure == "0" else 55, doc)
                    self.assertTrue((target / "recv.actual-pid").exists())
                    for actual in target.glob("*.actual-pid"):
                        pid = int(actual.read_text(encoding="utf-8"))
                        proc = Path(f"/proc/{pid}/stat")
                        self.assertTrue(not proc.exists() or proc.read_text().split()[2] == "Z",
                                        f"target process {pid} survived teardown")
                    self.assertIn("kill -0", log.read_text(encoding="utf-8"))
                finally:
                    for actual in target.glob("*.actual-pid"):
                        pid = int(actual.read_text(encoding="utf-8"))
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            continue
    # ---- P2-G: real media evidence, not marker presence (pre-fix FAILs) ----
    def test_video_requires_painted_frames(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_PUMP="1", FAKE_OBS_INPUT="1",
                                FAKE_FRAMES_PAINTED="0")
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "observable_missing")
        self.assertFalse(doc["observables"]["video_present"])

    def test_audio_chunks_without_pump_success_is_failed(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_AUDIO="1", FAKE_OBS_INPUT="1",
                                FAKE_OBS_PUMP="0")
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "observable_missing")
        self.assertFalse(doc["observables"]["audio_present"])

    def test_input_forwarded_without_ok_true_is_failed(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_PUMP="1", FAKE_INPUT_ANY="1",
                                FAKE_OBS_INPUT="0")
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "observable_missing")
        self.assertFalse(doc["observables"]["input_evidence"])

    def test_projecting_with_no_media_is_non_pass(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "projecting"', FAKE_ACTIVE="1",
                                FAKE_OBS_VIDEO="1", FAKE_OBS_AUDIO="1", FAKE_INPUT_ANY="1",
                                FAKE_OBS_INPUT="0", FAKE_OBS_PUMP="0", FAKE_FRAMES_PAINTED="0")
        self.assertEqual(code, 4, doc)
        self.assertNotEqual(doc["status"], "PASS")

    def test_projection_not_reached_is_failed(self):
        code, doc = self.run_qa(False, FAKE_STATE='string "idle"')
        self.assertEqual(code, 4, doc)
        self.assertEqual(doc["status"], "FAILED")
        self.assertEqual(doc["blocked_on"], "session_not_active")

    # ---- missing prerequisites stay BLOCKED ----
    def test_absent_phone_is_blocked(self):
        code, doc = self.run_qa(True, FAKE_PHONE=":")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        self.assertEqual(doc["blocked_on"], "phone_absent_on_port")

    def test_ssh_unreachable_is_blocked(self):
        code, doc = self.run_qa(True, FAKE_SSH_FAIL="1")
        self.assertEqual(code, 3, doc)
        self.assertEqual(doc["status"], "BLOCKED")
        self.assertEqual(doc["blocked_on"], "ssh_unreachable")

    # ---- P3: remote args escaped, host/IP values never land raw ----
    def test_remote_args_are_shell_escaped(self):
        # remote() runs its command string through the TARGET shell: a
        # metacharacter in --port must arrive escaped (literal), never execute.
        raw = "1-2.1;echo INJECTED"
        escaped = r"1-2.1\;echo\ INJECTED"
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            code, doc = self.run_qa(True, FAKE_SSH_LOG=str(log), PORT=raw)
            self.assertEqual(code, 0, doc)
            logged = log.read_text(encoding="utf-8")
            self.assertIn(escaped, logged)
            self.assertNotIn(raw, logged)

    def test_host_never_written_raw_to_artifact(self):
        sentinel = "sentinel-host-xyz"
        code, doc = self.run_qa(True, HOST=sentinel)
        self.assertEqual(code, 0, doc)
        dumped = json.dumps(doc)
        self.assertNotIn(sentinel, dumped)
        self.assertTrue(doc.get("host"), doc)
        self.assertNotEqual(doc["host"], sentinel)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: test_live_qa_gates.py <run-wired-live-qa.sh>")
    LiveQaGates.script = Path(sys.argv[1])
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(LiveQaGates)
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
