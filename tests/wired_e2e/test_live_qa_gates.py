#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wired_e2e/test_live_qa_gates.py <path-to-run-wired-live-qa.sh>
# noqa: SIZE_OK - one hermetic harness CLI regression suite; limited to this file by task scope.
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
      audio requires audio_chunks plus media_timing (timing alone can be
      emitted at final flush), input requires "ok":"true" (an ok:false
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
if [[ "$cmd" == *nohup* || "$cmd" == *kill* ]]; then
    if [[ "${FAKE_PROCESSES:-0}" == 1 ]]; then
        cmd="${cmd//\/tmp\//$FAKE_TARGET/}"
        cmd="${cmd//\/opt\/aa\/recv/$FAKE_TARGET/recv}"
        cmd="${cmd//\/opt\/aa\/disp/$FAKE_TARGET/disp}"
        if [[ "$cmd" == *nohup*disp* && "${FAKE_LAUNCH_FAILURE:-0}" == 1 ]]; then
            exit 55
        fi
        bash -euc "$cmd"
        exit $?
    fi
fi
if [ "$cmd" = "true" ]; then
    [ "${FAKE_SSH_FAIL:-0}" = "1" ] && exit 255
    exit 0
fi
if [[ "$cmd" == *nohup*--test-only-autostart* ]]; then
    [[ "${FAKE_DISPLAY_LAUNCH_FAIL:-0}" == 1 ]] && exit 41
    cmd="${cmd//\/tmp\//$FAKE_TARGET/}"
    # Execute the launch shell/redirections, validating the actual display CLI
    # and FIFO stdin rather than accepting any command containing 'nohup'.
    bash -euc 'nohup() {
        [[ "$#" == 3 && "$2" == --test-only-autostart && "$3" == --test-only-control-stdin ]]
        [[ -p /dev/stdin && "${FAKE_CONTROL_READY:-1}" == 1 ]]
        touch "$FAKE_TARGET/control-ready"
    }; '"$cmd"'; wait' >/dev/null
    echo 4343
    exit 0
fi
if [[ "$cmd" == *aa-disp.in* ]]; then
    [[ -f "$FAKE_TARGET/control-ready" && -p "$FAKE_TARGET/aa-disp.in" ]] || exit 97
    cmd="${cmd//\/tmp\//$FAKE_TARGET/}"
    # Keep a real FIFO reader open, then execute the exact remote writes/grep.
    # The status grep consumes the request before producing the fixture reply.
    bash -euc 'exec 9<>"$FAKE_TARGET/aa-disp.in"
        grep() {
            IFS= read -r -t 0.1 -u 9 request
            [[ "$request" == status ]]
            printf "{\"event\":\"status\",\"frames_presented\":3,\"frames_painted\":%s,\"render_ok\":%s}\n" \
                "${FAKE_FRAMES_PAINTED:-3}" "${FAKE_RENDER_OK:-true}" > "$FAKE_TARGET/aa-wired-display.log"
            command grep "$@"
        }
        '"$cmd"'
        if IFS= read -r -t 0.01 -u 9 press; then
            IFS= read -r -t 0.1 -u 9 release
            [[ "$press" == "key a press" && "$release" == "key a release" ]]
            command grep -q '\''"event":"active"'\'' "$FAKE_LOG_ROOT/receiver.log"
            count=0
            [[ -f "$FAKE_LOG_ROOT/keys" ]] && count="$(cat "$FAKE_LOG_ROOT/keys")"
            printf "%s" "$((count + 1))" > "$FAKE_LOG_ROOT/keys"
        fi'
    exit $?
fi
if [[ "$cmd" == *grep* ]]; then
    sleep "${FAKE_PROBE_DELAY_S:-0}"
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
        if [[ "${FAKE_FINAL_MEDIA_TIMING:-0}" == 1 || ( "${FAKE_OBS_AUDIO:-1}" == 1 && "${FAKE_OBS_PUMP:-1}" == 1 ) ]]; then
            printf '%s\n' '{"event":"media_timing","audio_latency_ms":1,"av_sync_ms":1}' >> "$log"
        fi
        keys=0
        [[ -f "$FAKE_LOG_ROOT/keys" ]] && keys="$(cat "$FAKE_LOG_ROOT/keys")"
        if [[ "${FAKE_OBS_INPUT:-1}" == 1 ]] && ((keys >= ${FAKE_INPUT_AFTER_KEYS:-1})); then
            printf '%s\n' "{\"event\":\"input_forwarded\",\"ok\":\"${FAKE_INPUT_OK:-true}\"}" >> "$log"
        fi
    fi
    bash -c "${cmd//\/tmp\/aa-wired-receiver.log/$log}"
    exit $?
fi
case "$cmd" in
    *idVendor*) printf '%s' "${FAKE_PHONE:-04e8:6860}"; exit 0 ;;
    *"/sys/class/drm"*) printf '%s' "${FAKE_DISPLAY:-connected}"; exit 0 ;;
    *xrandr*) printf '%s' "${FAKE_XRANDR-}"; exit "${FAKE_XRANDR_EXIT:-0}" ;;
    *pactl*|*pw-cli*) printf '%s' "${FAKE_SINK:-sink1}"; exit 0 ;;
    *70-android-auto-receiver.rules*) printf '%s' "${FAKE_POLICY:-installed}"; exit 0 ;;
    *nohup*) echo 4242; exit 0 ;;
    *busctl*) printf '%s' "${FAKE_STATE:-string \"projecting\"}"; exit 0 ;;
    *kill*)
        bash -c 'kill() { printf "kill %s\n" "$*" >> "$FAKE_SSH_LOG"; [[ "$1" != -0 ]]; }; '"$cmd"
        exit $? ;;
    *) exit 96 ;;
esac
"""


class LiveQaGates(unittest.TestCase):
    script = ""

    def run_qa(self, readiness_only: bool, *, projection_timeout_ms: int = 300,
               **env: str) -> tuple[int, dict]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            shim = root / "ssh"
            shim.write_text(FAKE_SSH, encoding="utf-8")
            shim.chmod(0o755)
            out = root / "result.json"
            command = [str(self.script), "--out", str(out), "--projection-timeout-ms",
                       str(projection_timeout_ms)]
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

    def test_xrandr_connected_passes_when_drm_is_disconnected(self) -> None:
        # Given a stale NVIDIA DRM connector and a live X output.
        output = "DP-0 connected primary 1920x1080+0+0 (normal left inverted right x axis y axis)"
        # When the real readiness CLI probes the scripted target.
        code, doc = self.run_qa(True, FAKE_DISPLAY="disconnected", FAKE_XRANDR=output)
        # Then readiness passes without claiming projection or delivery.
        self.assertEqual((code, doc["status"], doc["blocked_on"]), (0, "READY", ""))
        self.assertFalse(any(value for key, value in doc["observables"].items()
                             if key != "startup_budget_ms"))

    def test_xrandr_nonpositive_or_failed_probe_stays_blocked(self) -> None:
        # Given no positive DRM evidence and empty, negative, malformed, or failed X probes.
        for output, exit_code in (("", "0"), ("DP-0 disconnected", "0"),
                                  ("DP-0 connectedness", "0"), ("connected DP-0", "0"),
                                  ("", "1"), ("", "127"), ("DP-0 connected 1920x1080+0+0", "1")):
            with self.subTest(output=output, exit_code=exit_code):
                # When readiness runs with the probe's actual stdout and exit status.
                code, doc = self.run_qa(True, FAKE_DISPLAY="disconnected",
                                       FAKE_XRANDR=output, FAKE_XRANDR_EXIT=exit_code)
                # Then the existing fail-closed display verdict remains intact.
                self.assertEqual((code, doc["status"], doc["blocked_on"]),
                                 (3, "BLOCKED", "display_disconnected"))

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

    def test_video_requires_painted_frames_and_successful_render(self) -> None:
        # Given video_sent and presented frames, but no positive paint/render evidence.
        for frames, render in (("0", "true"), ("3", "false")):
            with self.subTest(frames=frames, render=render):
                # When the harness requests status over its real FIFO command.
                code, doc = self.run_qa(False, FAKE_FRAMES_PAINTED=frames,
                                       FAKE_RENDER_OK=render)
                # Then receiver-side video alone cannot qualify delivery.
                self.assertEqual((code, doc["status"], doc["blocked_on"]),
                                 (4, "FAILED", "observable_missing"))
                self.assertFalse(doc["observables"]["video_present"])

    def test_audio_chunks_without_successful_pump_is_failed(self) -> None:
        # Given successful delivery chunks but no media_timing measurement.
        # When the harness runs its normal delivery probes.
        code, doc = self.run_qa(False, FAKE_OBS_AUDIO="1", FAKE_OBS_PUMP="0")
        # Then chunks alone cannot satisfy both required audio observables.
        self.assertEqual((code, doc["status"], doc["blocked_on"]),
                         (4, "FAILED", "observable_missing"))
        self.assertFalse(doc["observables"]["audio_present"])

    def test_final_timing_after_failed_pump_without_chunks_is_failed(self) -> None:
        # Given a failed pump whose final timing export has no delivered chunks.
        # When all other delivery paths succeed through the normal CLI.
        code, doc = self.run_qa(False, FAKE_OBS_AUDIO="0", FAKE_OBS_PUMP="0",
                               FAKE_FINAL_MEDIA_TIMING="1")
        # Then timing alone must not produce the old false PASS.
        self.assertEqual((code, doc["status"], doc["blocked_on"]),
                         (4, "FAILED", "observable_missing"), doc)
        self.assertFalse(doc["observables"]["audio_present"])
        for observable in ("projection_reached", "video_present", "input_evidence"):
            self.assertTrue(doc["observables"][observable], doc)

    def test_audio_chunks_and_timing_qualify_delivery(self) -> None:
        # Given successful written frames and a periodic timing measurement.
        # When the normal CLI observes both records without a final export.
        code, doc = self.run_qa(False, FAKE_OBS_AUDIO="1", FAKE_OBS_PUMP="1",
                               FAKE_FINAL_MEDIA_TIMING="0")
        # Then real delivery still qualifies.
        self.assertEqual((code, doc["status"]), (0, "PASS"), doc)
        self.assertTrue(doc["observables"]["audio_present"])

    def test_control_channel_absent_cannot_qualify_input(self) -> None:
        # Given a display that never enables its test-only channel.
        # When the exact remote FIFO writes run without a control consumer.
        code, doc = self.run_qa(False, FAKE_CONTROL_READY="0")
        # Then no input delivery is invented by the SSH fixture.
        self.assertEqual((code, doc["status"]), (4, "FAILED"))
        self.assertFalse(doc["observables"]["input_evidence"])

    def test_fifo_launch_status_and_keys_retry_until_delivery(self) -> None:
        # Given input that succeeds only after a second key pair.
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            # When the normal harness launches and stimulates the display.
            code, doc = self.run_qa(False, FAKE_INPUT_AFTER_KEYS="2", FAKE_SSH_LOG=str(log))
            # Then the real FIFO/CLI contract was used, not an unconditional input marker.
            self.assertEqual((code, doc["status"]), (0, "PASS"))
            commands = log.read_text(encoding="utf-8")
            self.assertIn("--test-only-autostart --test-only-control-stdin <>/tmp/aa-disp.in", commands)
            self.assertIn(r"printf 'status\n' > /tmp/aa-disp.in", commands)
            self.assertGreaterEqual(commands.count(r"printf 'key a press\nkey a release\n'"), 2)

    def test_input_is_not_stimulated_before_active(self) -> None:
        # Given a projecting control service without an ACTIVE phone session.
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "ssh.log"
            # When delivery polling reaches its unchanged short test deadline.
            code, doc = self.run_qa(False, FAKE_ACTIVE="0", FAKE_SSH_LOG=str(log))
            # Then no premature key events or input delivery are claimed.
            self.assertEqual((code, doc["blocked_on"]), (4, "session_not_active"))
            self.assertNotIn("key a press", log.read_text(encoding="utf-8"))
            self.assertFalse(doc["observables"]["input_evidence"])

    def test_control_fixture_rejects_invalid_flag_or_non_fifo_stdin(self) -> None:
        # Given incorrect display CLI flags or a regular file in place of the FIFO.
        for flag, fifo in (("--test-only-control", True), ("", True),
                           ("--test-only-control-stdin", False)):
            with self.subTest(flag=flag, fifo=fifo), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                shim = root / "ssh"
                shim.write_text(FAKE_SSH, encoding="utf-8")
                env = dict(os.environ, FAKE_TARGET=directory)
                session = ('export DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus; '
                           'export DISPLAY=:1;')
                setup = "mkfifo" if fifo else "touch"
                launch = (f"{setup} /tmp/aa-disp.in; nohup /opt/aa/disp --test-only-autostart "
                          f"{flag} <>/tmp/aa-disp.in > /tmp/aa-wired-display.log 2>&1 & echo $!")
                # When the same fixture executes launch redirection and a status request.
                subprocess.run(["bash", str(shim), session, launch], env=env,
                               capture_output=True, timeout=TIMEOUT_S, check=True)
                result = subprocess.run(
                    ["bash", str(shim), session, "printf 'status\n' > /tmp/aa-disp.in"],
                    env=env, capture_output=True, timeout=TIMEOUT_S,
                )
                # Then the invalid launch cannot manufacture control-channel evidence.
                self.assertEqual(result.returncode, 97)

    def test_delayed_streams_and_activation_are_polled(self) -> None:
        # This checks eventual evidence, not a 300 ms host scheduling budget.
        with tempfile.TemporaryDirectory() as directory:
            code, doc = self.run_qa(
                False, projection_timeout_ms=1500, FAKE_LOG_ROOT=directory,
                FAKE_DELAY_POLLS="4", FAKE_PROBE_DELAY_S="0.04",
            )
            self.assertEqual(code, 0, doc)
            self.assertEqual(doc["status"], "PASS")
            self.assertGreater(int((Path(directory) / "polls").read_text()), 4)

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

    def test_final_flush_timing_without_delivered_audio_is_failed(self) -> None:
        code, doc = self.run_qa(False, FAKE_OBS_AUDIO="0", FAKE_OBS_PUMP="1",
                               FAKE_FINAL_MEDIA_TIMING="1")
        self.assertEqual(code, 4, doc)
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

    def test_target_pids_are_terminated_on_success_and_launch_failure(self) -> None:
        for failure in ("0", "1"):
            with self.subTest(launch_failure=failure), tempfile.TemporaryDirectory() as directory:
                target = Path(directory)
                for name in ("recv", "disp"):
                    program = target / name
                    program.write_text(
                        '#!/usr/bin/env bash\nset -euo pipefail\n'
                        'printf "%s\\n" "$$" > "${0}.actual-pid"\n'
                        'if [[ "$0" == */disp ]]; then\n'
                        '  [[ "$#" == 2 && "$1" == --test-only-autostart && "$2" == --test-only-control-stdin && -p /dev/stdin ]] || exit 2\n'
                        '  touch "$FAKE_TARGET/control-ready"\n'
                        'fi\nexec sleep 60\n',
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
