# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B -m unittest discover -s tests/wired_qa -v
"""#137 drive smoke: analyzer, pass/fail logic and the soak gate, on fixture logs.

The fixture lines copy the shape of the live receiver/display logs (2026-11-16
A26 runs) with synthetic times; they carry no phone identifiers.
"""
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from datetime import datetime, timedelta, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SMOKE = ROOT / "tools" / "wired-qa" / "aa_drive_smoke.py"
LAUNCHER = ROOT / "tools" / "perf" / "aa-projection-perf.sh"
sys.path.insert(0, str(SMOKE.parent))
import aa_drive_smoke as smoke  # noqa: E402

S = 10**9
ACTIVE = 1_000 * S


class Run:
    """Writes one runner output directory (receiver.log, display.log, idr-events.jsonl, run.json)."""

    def __init__(self, fps: int = 30, painted_fps: float | None = None, duration: int = 600,
                 width: int = 1280, height: int = 720):
        self.fps, self.duration = fps, duration
        self.first = ACTIVE + 2 * S
        self.receiver = ["Opening in BLOCKING MODE ",
                         {"event": "ready"}, {"event": "active"},
                         {"event": "video_profile", "width": width, "height": height, "fps": fps,
                          "codec": "h264"}]
        self.display: list = [{"event": "ready"}]
        self.stamped: list = []
        self.run = {"duration_s": duration, "active_ns": ACTIVE,
                    "window_end_ns": self.first + duration * S + S // 2,
                    "receiver_alive_at_end": True, "display_alive_at_end": True}
        rate = painted_fps or fps
        count = int((duration + 1) * rate)
        self.paints = [self.first + int(i * S / rate) for i in range(count)]
        self.av_delay_ns = None

    def freeze(self, at_s: float, seconds: float) -> "Run":
        lo, hi = self.first + int(at_s * S), self.first + int((at_s + seconds) * S)
        self.paints = [t for t in self.paints if not lo <= t < hi]
        return self

    def idr(self, source: str, at_s: float, seconds: float | None, latency_ms: int | None = None):
        start = self.first + int(at_s * S)
        self.stamped.append({"observed_ns": start, "event": {
            "event": "idr_requested", "source": source, "reason": "waiting_idr"}})
        if seconds is not None:
            self.stamped.append({"observed_ns": start + int(seconds * S), "event": {
                "event": "idr_recovered", "source": source,
                "latency_ms": latency_ms if latency_ms is not None else int(seconds * 1000),
                "episode_requests": 1, "total_requests": 1}})
        return self

    def media(self, av_delay_ms: float) -> "Run":
        self.av_delay_ns = int(av_delay_ms * 1e6)
        return self

    def write(self, directory: Path) -> Path:
        display = list(self.display)
        for frame, t in enumerate(self.paints):
            display.append({"event": "video_presented", "frame": frame, "scheduled_ns": t - 30_000_000,
                            "completed_ns": t, "count": 1})
        receiver = list(self.receiver)
        if self.av_delay_ns is not None:
            slot = 42_666_666
            start = self.first - S
            while start < self.paints[-1]:
                receiver.append({"event": "audio_write_completed", "scheduled_ns": start,
                                 "scheduled_end_ns": start + slot, "completed_ns": start,
                                 "presented_ns": start + 30_000_000 + self.av_delay_ns})
                start += slot
        for name, lines in (("receiver.log", receiver), ("display.log", display)):
            (directory / name).write_text("\n".join(
                line if isinstance(line, str) else json.dumps(line, separators=(",", ":"))
                for line in lines) + "\n")
        (directory / "idr-events.jsonl").write_text("".join(json.dumps(x) + "\n" for x in self.stamped))
        (directory / "run.json").write_text(json.dumps(self.run))
        return directory


def criterion(result: dict, name: str) -> dict:
    return next(c for c in result["criteria"] if c["name"] == name)


class AnalyzerTests(unittest.TestCase):
    def analyze(self, run: Run) -> dict:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-") as directory:
            return smoke.analyze_dir(run.write(Path(directory)))

    def test_healthy_default_profile_passes_every_window(self) -> None:
        # Given ten minutes of 720p30 painting at the negotiated rate.
        result = self.analyze(Run())
        # Then the 59 windows after the 10 s startup grace clear the 27 fps floor;
        # A/V is not valid without media, so it is reported n/a and not failed.
        fps = criterion(result, "painted_fps_per_window")
        self.assertEqual(result["verdict"], "PASS", result["criteria"])
        self.assertEqual((fps["windows"], fps["expected_windows"], fps["floor_fps"]), (59, 59, 27.0))
        self.assertIsNone(criterion(result, "av_sync_within_ceiling")["passed"])

    def test_one_second_freeze_fails_its_window(self) -> None:
        # Given a 1.2 s paint gap 305 s into the drive (the #134 shape).
        result = self.analyze(Run().freeze(305.0, 1.2))
        # Then exactly that window falls below 90 % of 30 fps.
        fps = criterion(result, "painted_fps_per_window")
        self.assertEqual(result["verdict"], "FAIL")
        self.assertEqual([w["start_offset_s"] for w in fps["failing_windows"]], [300])

    def test_startup_grace_covers_window_zero_only(self) -> None:
        # Given the 2026-11-16 A26 startup shape: the phone encoder pauses just
        # after the first frame, leaving 26.4 fps in that 10 s window.
        early = self.analyze(Run().freeze(0.1, 1.2))
        # Then the fps startup grace excludes it...
        self.assertEqual(early["verdict"], "PASS", early["criteria"])
        self.assertEqual(criterion(early, "painted_fps_per_window")["startup_grace_s"], 10)
        # ...but the same dip in window 5 (50-60 s) fails.
        later = self.analyze(Run().freeze(50.1, 1.2))
        self.assertEqual(later["verdict"], "FAIL")
        self.assertEqual([w["start_offset_s"] for w in
                          criterion(later, "painted_fps_per_window")["failing_windows"]], [50])

    def test_other_criteria_still_cover_the_grace(self) -> None:
        # Given a 1.5 s waiting_idr episode inside the fps grace.
        result = self.analyze(Run().idr("consumer", 2.0, 1.5))
        # Then it still fails the run.
        self.assertFalse(criterion(result, "waiting_idr_max")["passed"])

    def test_sixty_fps_profile_painting_fifty_fails(self) -> None:
        # Given today's healthy 1080p60 throughput (about 50 painted fps, #79/#85).
        result = self.analyze(Run(fps=60, painted_fps=50, width=1920, height=1080))
        # Then the unchanged 90 % rule fails it: the gate does not qualify 60 fps.
        self.assertFalse(criterion(result, "painted_fps_per_window")["passed"])
        self.assertEqual(criterion(result, "painted_fps_per_window")["floor_fps"], 54.0)

    def test_waiting_idr_longer_than_one_second_fails(self) -> None:
        for episode, passed in (((0.4, None), True), ((1.5, None), False),
                                ((0.3, 1200), False), ((None, None), False)):
            with self.subTest(episode=episode):
                seconds, latency = episode
                result = self.analyze(Run().idr("producer", 100.0, seconds, latency))
                self.assertIs(criterion(result, "waiting_idr_max")["passed"], passed, result["criteria"])

    def test_waiting_idr_after_the_window_is_shutdown_not_drive(self) -> None:
        # Given the producer waiting for an IDR only after the window ended
        # (the display is stopped first during cleanup).
        result = self.analyze(Run().idr("producer", 601.0, None))
        self.assertTrue(criterion(result, "waiting_idr_max")["passed"])

    def test_session_drop_inside_the_window_fails(self) -> None:
        # Given a disconnect 200 s in, a dead display, and a disconnect at shutdown.
        inside = Run()
        inside.receiver.append({"event": "diag", "kind": "session_state_changed", "session": 1,
                                "mono_ns": inside.first + 200 * S, "to_state": "disconnected",
                                "transport": "usb"})
        dead = Run()
        dead.run["display_alive_at_end"] = False
        short = Run()
        short.run["window_end_ns"] = short.first + 300 * S
        shutdown = Run()
        shutdown.receiver.append({"event": "diag", "kind": "session_state_changed", "session": 1,
                                  "mono_ns": shutdown.first + 601 * S, "to_state": "disconnected",
                                  "transport": "usb"})
        for run, passed in ((inside, False), (dead, False), (short, False), (shutdown, True)):
            with self.subTest(passed=passed):
                self.assertIs(criterion(self.analyze(run), "no_session_drop")["passed"], passed)

    def test_av_is_gated_by_the_existing_ceiling_when_media_plays(self) -> None:
        good = self.analyze(Run().media(12.0))
        bad = self.analyze(Run().media(65.0))
        self.assertTrue(criterion(good, "av_sync_within_ceiling")["passed"])
        self.assertEqual(criterion(good, "av_sync_within_ceiling")["ceiling_ms"], 50.0)
        self.assertFalse(criterion(bad, "av_sync_within_ceiling")["passed"])
        self.assertEqual(bad["verdict"], "FAIL")

    def test_first_paint_latency_is_reported_not_gated(self) -> None:
        # Given the first paint 22 s after active (there is no first-paint deadline).
        run = Run()
        run.paints = [t + 20 * S for t in run.paints]
        run.run["window_end_ns"] += 20 * S
        result = self.analyze(run)
        self.assertEqual(result["verdict"], "PASS", result["criteria"])
        self.assertAlmostEqual(result["measured"]["startup_to_first_paint_s"], 22.0)

    def test_no_paint_at_all_fails(self) -> None:
        run = Run()
        run.paints = []
        result = self.analyze(run)
        self.assertEqual(result["verdict"], "FAIL")
        self.assertIn("reason", criterion(result, "painted_fps_per_window"))

    def test_recovery_without_a_logged_request_still_counts(self) -> None:
        # Given consumer recoveries whose requests all failed (none logged), as
        # DisplayController emits them: one reports 1500 ms, one no latency.
        with_latency, without = Run(), Run()
        with_latency.stamped.append({"observed_ns": with_latency.first + 3 * S, "event": {
            "event": "idr_recovered", "source": "consumer", "latency_ms": 1500,
            "episode_requests": 0, "total_requests": 0}})
        without.stamped.append({"observed_ns": without.first + 3 * S, "event": {
            "event": "idr_recovered", "source": "consumer"}})
        # Then both fail: the first by its own latency, the second as open since active.
        first = criterion(self.analyze(with_latency), "waiting_idr_max")
        self.assertFalse(first["passed"])
        self.assertAlmostEqual(first["longest_s"], 1.5)
        second = criterion(self.analyze(without), "waiting_idr_max")
        self.assertFalse(second["passed"])
        self.assertAlmostEqual(second["longest_s"], 5.0)

    def recovery(self, request_s: float | None, recovered_s: float, **fields) -> dict:
        """Analyze a run with one consumer episode, times relative to `active`."""
        run = Run()
        if request_s is not None:
            run.stamped.append({"observed_ns": ACTIVE + int(request_s * S), "event": {
                "event": "idr_requested", "source": "consumer", "reason": "video_consumer_waiting_idr"}})
        run.stamped.append({"observed_ns": ACTIVE + int(recovered_s * S), "event": {
            "event": "idr_recovered", "source": "consumer", **fields}})
        return criterion(self.analyze(run), "waiting_idr_max")

    def test_unusable_latency_starts_at_active_even_with_a_request(self) -> None:
        # Given a request 0.2 s before recovery at active+5 s: the wait may
        # predate the request, so without a usable latency it counts from active.
        for latency in ({}, {"latency_ms": -1}, {"latency_ms": float("nan")},
                        {"latency_ms": float("inf")}, {"latency_ms": "200"}, {"latency_ms": True},
                        {"latency_ms": None}):
            with self.subTest(latency=latency):
                idr = self.recovery(4.8, 5.0, **latency)
                self.assertFalse(idr["passed"], idr)
                self.assertAlmostEqual(idr["longest_s"], 5.0, places=3)
        # A usable latency establishes the start (control).
        idr = self.recovery(4.8, 5.0, latency_ms=200)
        self.assertTrue(idr["passed"], idr)
        self.assertAlmostEqual(idr["longest_s"], 0.2, places=3)

    def test_episode_is_clipped_at_active(self) -> None:
        # A recovery at active+0.5 s with latency 1500 counts only its 0.5 s after active.
        idr = self.recovery(None, 0.5, latency_ms=1500)
        self.assertTrue(idr["passed"], idr)
        self.assertAlmostEqual(idr["longest_s"], 0.5, places=3)
        # An episode wholly before active counts 0.
        idr = self.recovery(-3.0, -2.0, latency_ms=1500)
        self.assertTrue(idr["passed"], idr)
        self.assertEqual(idr["longest_s"], 0.0)

    def test_episode_is_clipped_to_the_drive_window(self) -> None:
        # Given a request 0.2 s before the drive ends that recovers 2 s later.
        result = self.analyze(Run().idr("producer", 599.8, 2.0))
        # Then only the 0.2 s inside the window counts.
        idr = criterion(result, "waiting_idr_max")
        self.assertTrue(idr["passed"], idr)
        self.assertAlmostEqual(idr["longest_s"], 0.2, places=3)

    def test_null_audio_endpoint_is_excluded_and_counted(self) -> None:
        # Given the live run-2 shape: one audio slot whose presented_ns is null.
        run = Run().media(12.0)
        run.receiver.append({"event": "audio_write_completed", "scheduled_ns": run.first,
                             "scheduled_end_ns": run.first + 42_666_666,
                             "completed_ns": run.first, "presented_ns": None})
        av = criterion(self.analyze(run), "av_sync_within_ceiling")
        # Then the slot is skipped ("where valid") but reported.
        self.assertTrue(av["passed"])
        self.assertEqual(av["invalid_audio_slots"], 1)


NOW = datetime(2026, 11, 16, 12, tzinfo=timezone.utc)
A, B = "a" * 64, "b" * 64


def record(finished: datetime = NOW - timedelta(hours=2), **changes) -> dict:
    doc = {"schema": smoke.SCHEMA, "verdict": "PASS", "duration_s": 600, "video_profiles": None,
           "finished_utc": finished.isoformat(),
           "binaries": {"receiver": {"sha256": A}, "display": {"sha256": B}}}
    doc.update(changes)
    return doc


def run_check(path: Path, receiver: str = A, display: str = B,
              now: str | None = "2026-11-16T12:00:00Z") -> subprocess.CompletedProcess:
    command = [sys.executable, "-B", str(SMOKE), "check", "--record", str(path),
               "--receiver-sha256", receiver, "--display-sha256", display]
    return subprocess.run(command + (["--now", now] if now else []),
                          capture_output=True, text=True, check=False)


class GateTests(unittest.TestCase):
    def check(self, doc, receiver=A, display=B):
        return smoke.check_record(doc, receiver_sha256=receiver, display_sha256=display, now=NOW)[0]

    def test_fresh_pass_for_the_same_bytes_admits_the_soak(self) -> None:
        self.assertIsNone(self.check(record()))

    def test_refusals_are_named(self) -> None:
        cases = {
            "drive_smoke_missing": [None, {"schema": "other"}],
            "drive_smoke_not_passed": [record(verdict="FAIL"), record(verdict="BLOCKED")],
            "drive_smoke_stale": [record(NOW - timedelta(hours=25)), record(NOW + timedelta(hours=1))],
            "drive_smoke_profile": [record(video_profiles="1920x1080@60:h264")],
            "drive_smoke_short": [record(duration_s=120)],
        }
        for reason, docs in cases.items():
            for doc in docs:
                with self.subTest(reason=reason, doc=doc):
                    self.assertEqual(self.check(doc), reason)
        self.assertEqual(self.check(record(), receiver="c" * 64), "drive_smoke_build_mismatch")
        self.assertEqual(self.check(record(), display="c" * 64), "drive_smoke_build_mismatch")

    def test_malformed_and_nonfinite_fields_are_refused(self) -> None:
        bad = [record(duration_s=float("nan")), record(duration_s=float("inf")),
               record(duration_s="600"), record(duration_s=True), record(duration_s=None),
               record(verdict=None), record(finished_utc="2026-11-16"),
               record(finished_utc="yesterday"), record(finished_utc=0),
               record(video_profiles=0), record(binaries=[]),
               record(binaries={"receiver": {"sha256": A}}),
               record(binaries={"receiver": {"sha256": A}, "display": {"sha256": "B" * 64}})]
        for doc in bad:
            with self.subTest(doc=doc):
                self.assertEqual(self.check(doc), "drive_smoke_malformed")

    def test_missing_video_profiles_key_is_malformed(self) -> None:
        doc = record()
        del doc["video_profiles"]
        self.assertEqual(self.check(doc), "drive_smoke_malformed")
        with tempfile.TemporaryDirectory(prefix="drive-smoke-gate-") as directory:
            path = Path(directory) / "result.json"
            path.write_text(json.dumps(doc))
            result = run_check(path)
        self.assertEqual((result.returncode, result.stdout.strip()), (3, "drive_smoke_malformed"))
        self.assertNotIn("Traceback", result.stderr)

    def test_both_launched_hashes_are_required(self) -> None:
        for receiver, display in (("", B), (A, ""), (A, None), (A, "xyz"), (A, B.upper())):
            with self.subTest(receiver=receiver, display=display):
                self.assertEqual(self.check(record(), receiver, display), "drive_smoke_hash_failed")

    def test_cli_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-gate-") as directory:
            path = Path(directory) / "result.json"
            path.write_text(json.dumps(record()))
            ok, bad = run_check(path), run_check(path, receiver="c" * 64)
            empty = run_check(path, display="")
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertEqual((bad.returncode, bad.stdout.strip()), (3, "drive_smoke_build_mismatch"))
        self.assertEqual((empty.returncode, empty.stdout.strip()), (3, "drive_smoke_hash_failed"))

    def test_cli_refuses_nan_and_infinity_json(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-gate-") as directory:
            path = Path(directory) / "result.json"
            for constant in ("NaN", "Infinity", "-Infinity"):
                with self.subTest(constant=constant):
                    path.write_text(json.dumps(record()).replace('"duration_s": 600',
                                                                 f'"duration_s": {constant}'))
                    result = run_check(path)
                    self.assertEqual(result.returncode, 3, result.stderr)
                    self.assertEqual(result.stdout.strip(), "drive_smoke_malformed", result.stderr)


class LauncherGateTests(unittest.TestCase):
    """The perf launcher refuses every live stage before the collector starts."""

    def launch(self, root: Path, *extra: str, fail_display_hash: bool = False
               ) -> tuple[subprocess.CompletedProcess, str]:
        stub_dir = root / "bin"
        stub_dir.mkdir(exist_ok=True)
        log = root / "ssh.log"
        log.write_text("")
        (stub_dir / "ssh").write_text(f'''#!/usr/bin/env bash
printf '%s\\n' "$*" >> {str(log)!r}
case "$*" in
    *"python3 -"*) exit 1 ;;
    *sha256sum*aa-wired-receiver*) printf '%s  r\\n' {A}; exit 0 ;;
    *sha256sum*aa-dummy-display*) [ -n "${{FAIL_DISPLAY_HASH:-}}" ] && exit 1
                                  printf '%s  d\\n' {B}; exit 0 ;;
    *xrandr* | *drm*) printf 'connected\\n'; exit 0 ;;
    *pactl* | *pw-cli*) printf '0\\tfake\\tPipeWire\\n'; exit 0 ;;
esac
exit 0
''')
        (stub_dir / "ssh").chmod(0o755)
        env = {"PATH": f"{stub_dir}:/usr/bin:/bin", "AA_PERF_HOST": "fake.local", "AA_DISPLAY": ":0"}
        if fail_display_hash:
            env["FAIL_DISPLAY_HASH"] = "1"
        process = subprocess.run(
            ["bash", str(LAUNCHER), "--mode", "wired", "--duration", "60s",
             "--out", str(root / "out.json"), *extra],
            capture_output=True, text=True, timeout=120, check=False, env=env)
        return process, log.read_text()

    def assert_blocked(self, root: Path, reason: str, *extra: str, **kwargs) -> None:
        process, ssh = self.launch(root, *extra, **kwargs)
        self.assertEqual(process.returncode, 3, process.stderr)
        self.assertEqual(json.loads((root / "out.json").read_text())["blocked_on"], reason,
                         process.stderr)
        self.assertNotIn("python3 -", ssh, "the stage started without a valid drive smoke")

    def write(self, root: Path, name: str, doc: dict) -> str:
        path = root / name
        path.write_text(json.dumps(doc))
        return str(path)

    def test_no_or_wrong_smoke_blocks_before_the_collector(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            now = datetime.now(timezone.utc)
            self.assert_blocked(root, "drive_smoke_missing")
            mismatch = record(now, binaries={"receiver": {"sha256": "c" * 64}, "display": {"sha256": B}})
            self.assert_blocked(root, "drive_smoke_build_mismatch",
                                "--drive-smoke", self.write(root, "mismatch.json", mismatch))

    def test_unhashable_or_absent_display_blocks(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            good = self.write(root, "good.json", record(datetime.now(timezone.utc)))
            # Given a display the target cannot hash (sha256sum fails for it only).
            self.assert_blocked(root, "drive_smoke_hash_failed", "--drive-smoke", good,
                                fail_display_hash=True)
            # Given no display configured at all.
            self.assert_blocked(root, "drive_smoke_hash_failed", "--drive-smoke", good,
                                "--display", "")

    def test_live_freshness_ignores_now(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            finished = datetime.now(timezone.utc) - timedelta(hours=30)
            old = self.write(root, "old.json", record(finished))
            # Given --now set to an hour after the stale smoke: live stays on the real clock.
            near = (finished + timedelta(hours=1)).strftime("%Y-%m-%dT%H:%M:%SZ")
            self.assert_blocked(root, "drive_smoke_stale", "--drive-smoke", old, "--now", near)

    def test_matching_smoke_reaches_the_collector(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            good = self.write(root, "good.json", record(datetime.now(timezone.utc)))
            process, ssh = self.launch(root, "--drive-smoke", good)
            # The stub collector fails, so the stage is BLOCKED on that, not on the gate.
            self.assertEqual(json.loads((root / "out.json").read_text())["blocked_on"],
                             "collector_failed", process.stderr)
            self.assertIn("python3 -", ssh)


FAKE_RECEIVER = """#!/usr/bin/env python3
import os, subprocess, time
directory = os.environ["FAKE_DIR"]
grandchild = subprocess.Popen(["sleep", "300"])
with open(os.path.join(directory, "receiver.pids"), "w") as handle:
    handle.write(f"{os.getpid()} {grandchild.pid}")
print('{"event":"ready"}', flush=True)
time.sleep(0.5)
print('{"event":"active"}', flush=True)
print('{"event":"video_profile","width":1280,"height":720,"fps":30,"codec":"h264"}', flush=True)
time.sleep(300)
"""

FAKE_DISPLAY = """#!/usr/bin/env python3
import os, sys, time
with open(os.path.join(os.environ["FAKE_DIR"], "display.pids"), "w") as handle:
    handle.write(str(os.getpid()))
print('{"event":"video_presented","frame":1,"scheduled_ns":%d,"completed_ns":%d,"count":1}'
      % (time.monotonic_ns() - 30000000, time.monotonic_ns()), flush=True)
if not os.environ.get("FAKE_DISPLAY_STALL"):
    for _ in sys.stdin:
        pass
time.sleep(300)
"""

FAKE_ADB = """#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$FAKE_DIR/adb.log"
case "$*" in
    "devices -l") printf 'List of devices attached\\nFAKESERIAL device usb:9-9 model:FAKE\\n' ;;
esac
exit 0
"""

# Run the real runner in a child Python with only the host-wide process scan
# stubbed (a compiler elsewhere on this machine must not block the test).
# DRIVE_HOOK reproduces the two re-review races:
#   spawn    SIGTERM queued inside Popen, right after the receiver exists and
#            before the runner has its handle (records the child's SigBlk);
#   cleanup  SIGINT delivered at cleanup entry (first SIG_IGN install or first
#            child stop), after a mid-drive SIGTERM.
DRIVER = """import os, signal, subprocess, sys, time
sys.path.insert(0, sys.argv[1])
import aa_drive_smoke
aa_drive_smoke._comms = lambda: []
hook, fake = os.environ.get("DRIVE_HOOK", ""), os.environ["FAKE_DIR"]
fired = []
if hook == "spawn":
    class Hooked(subprocess.Popen):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            if kwargs.get("start_new_session") and not fired:
                fired.append(self.pid)
                pids = os.path.join(fake, "receiver.pids")
                deadline = time.monotonic() + 10
                while not os.path.exists(pids) and time.monotonic() < deadline:
                    time.sleep(0.05)
                status = open(f"/proc/{self.pid}/status").read()
                with open(os.path.join(fake, "sigblk"), "w") as handle:
                    handle.write(next(l for l in status.splitlines() if l.startswith("SigBlk:")))
                os.kill(os.getpid(), signal.SIGTERM)
    subprocess.Popen = Hooked
elif hook == "cleanup":
    # Fire at whichever cleanup entry point comes first: a handler change to
    # SIG_IGN (the gap the re-review reproduced) or stopping the first child.
    def second_signal():
        if not fired:
            fired.append(1)
            os.kill(os.getpid(), signal.SIGINT)
    real_stop, real_signal = aa_drive_smoke._stop, signal.signal
    def hooked_stop(process):
        second_signal()
        real_stop(process)
    def hooked_signal(signum, handler):
        previous = real_signal(signum, handler)
        if handler is signal.SIG_IGN:
            second_signal()
        return previous
    aa_drive_smoke._stop = hooked_stop
    aa_drive_smoke.signal.signal = hooked_signal
elif hook == "stall":
    # The fake display never reads stdin (FAKE_DISPLAY_STALL). Shrink its pipe
    # and fill it, then hand the runner a blocking fd, as a stalled GUI would.
    import fcntl
    real_spawn = aa_drive_smoke._spawn
    def hooked_spawn(children, command, **kwargs):
        process = real_spawn(children, command, **kwargs)
        if kwargs.get("stdin") is subprocess.PIPE:
            fd = process.stdin.fileno()
            fcntl.fcntl(fd, 1031, 4096)  # F_SETPIPE_SZ
            os.set_blocking(fd, False)
            try:
                while True:
                    os.write(fd, b"x" * 4096)
            except BlockingIOError:
                pass
            os.set_blocking(fd, True)
            open(os.path.join(fake, "pipe.full"), "w").close()
        return process
    aa_drive_smoke._spawn = hooked_spawn
sys.exit(aa_drive_smoke.main(sys.argv[2:]))
"""


def alive(pid: int) -> bool:
    try:
        state = Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0]
    except (OSError, IndexError):
        return False
    return state != "Z"


class RunnerSignalTests(unittest.TestCase):
    """SIGTERM/SIGINT only set a flag; the runner always reaches its one cleanup."""

    def start(self, root: Path, hook: str = "") -> subprocess.Popen:
        build, bin_dir = root / "build", root / "bin"
        build.mkdir()
        bin_dir.mkdir()
        (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        for path, body in ((build / "aa-wired-receiver", FAKE_RECEIVER),
                           (build / "aa-dummy-display", FAKE_DISPLAY), (bin_dir / "adb", FAKE_ADB)):
            path.write_text(body)
            path.chmod(0o755)
        (root / "store.json").write_text("{}")
        env = dict(os.environ, FAKE_DIR=str(root), DRIVE_HOOK=hook, PATH=f"{bin_dir}:/usr/bin:/bin")
        if hook == "stall":
            env["FAKE_DISPLAY_STALL"] = "1"
        return subprocess.Popen(
            [sys.executable, "-B", "-c", DRIVER, str(SMOKE.parent), "run",
             "--serial", "FAKESERIAL", "--port", "9-9", "--build-dir", str(build),
             "--store", str(root / "store.json"), "--display-env", ":99",
             "--out", str(root / "out")], env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True)

    def wait_for(self, *paths: Path) -> None:
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline and not all(p.exists() for p in paths):
            time.sleep(0.1)
        self.assertTrue(all(p.exists() for p in paths), paths)

    def pids(self, root: Path) -> list[int]:
        out = [int(x) for x in (root / "receiver.pids").read_text().split()]
        if (root / "display.pids").exists():
            out.append(int((root / "display.pids").read_text()))
        return out

    def finish(self, root: Path, runner: subprocess.Popen, signum: int) -> None:
        """Then: exit 128+signum, every child and grandchild gone, USB restored, result written."""
        output, _ = runner.communicate(timeout=60)
        pids = self.pids(root)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and any(alive(pid) for pid in pids):
            time.sleep(0.1)
        self.assertEqual(runner.returncode, 128 + signum, output)
        self.assertFalse([pid for pid in pids if alive(pid)], output)
        result = json.loads((root / "out" / "result.json").read_text())
        self.assertEqual((result["verdict"], result["blocked_on"]), ("BLOCKED", "interrupted"))
        adb = (root / "adb.log").read_text().splitlines()
        self.assertEqual(adb[-1], "-s FAKESERIAL shell svc usb setFunctions mtp", adb)

    def scenario(self, hook: str, external_sigterm: bool, check) -> None:
        with tempfile.TemporaryDirectory(prefix=f"drive-smoke-signal-{hook or 'mid'}-") as directory:
            root = Path(directory)
            runner = self.start(root, hook)
            try:
                check(root, runner)
            finally:
                if runner.poll() is None:
                    runner.kill()
                for pid in (self.pids(root) if (root / "receiver.pids").exists() else []):
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_sigterm_mid_drive_stops_both_children_and_their_groups(self) -> None:
        def check(root: Path, runner: subprocess.Popen) -> None:
            # Given the runner mid-drive with both fake children running.
            self.wait_for(root / "receiver.pids", root / "display.pids")
            time.sleep(1.5)
            self.assertTrue(all(alive(pid) for pid in self.pids(root)))
            # When it is sent SIGTERM.
            runner.send_signal(signal.SIGTERM)
            self.finish(root, runner, signal.SIGTERM)
        self.scenario("", True, check)

    def test_signal_queued_right_after_child_creation(self) -> None:
        def check(root: Path, runner: subprocess.Popen) -> None:
            # Given SIGTERM arriving inside Popen, before _spawn returns the handle.
            self.finish(root, runner, signal.SIGTERM)
            # Then the receiver was still tracked and stopped; the display never started;
            # and the child ran with the stop signals unblocked.
            self.assertFalse((root / "display.pids").exists())
            mask = int((root / "sigblk").read_text().split()[1], 16)
            self.assertEqual(mask & ((1 << (signal.SIGTERM - 1)) | (1 << (signal.SIGINT - 1))), 0)
        self.scenario("spawn", False, check)

    def test_second_signal_during_cleanup_entry(self) -> None:
        def check(root: Path, runner: subprocess.Popen) -> None:
            # Given SIGTERM mid-drive and SIGINT as cleanup stops its first child.
            self.wait_for(root / "receiver.pids", root / "display.pids")
            time.sleep(1.5)
            runner.send_signal(signal.SIGTERM)
            # Then cleanup still completes and the first signal is the one reported.
            self.finish(root, runner, signal.SIGTERM)
        self.scenario("cleanup", True, check)

    def test_sigterm_while_display_control_pipe_is_full(self) -> None:
        def check(root: Path, runner: subprocess.Popen) -> None:
            # Given a display that stopped draining its full control pipe.
            self.wait_for(root / "receiver.pids", root / "display.pids", root / "pipe.full")
            time.sleep(0.5)
            # When the runner is sent SIGTERM mid-write.
            runner.send_signal(signal.SIGTERM)
            # Then it still reaches cleanup promptly.
            self.finish(root, runner, signal.SIGTERM)
        self.scenario("stall", True, check)

    def test_stalled_display_control_fails_the_run(self) -> None:
        def check(root: Path, runner: subprocess.Popen) -> None:
            # Given a full control pipe and no stop, the run ends as FAIL within the stall timeout.
            output, _ = runner.communicate(timeout=60)
            self.assertEqual(runner.returncode, 1, output)
            result = json.loads((root / "out" / "result.json").read_text())
            self.assertEqual(result["failure"], "display control input stalled")
            self.assertEqual(result["verdict"], "FAIL")
            self.assertFalse([pid for pid in self.pids(root) if alive(pid)], output)
        self.scenario("stall", False, check)


class SendControlTests(unittest.TestCase):
    def test_full_pipe_returns_stalled_or_stopped_without_blocking(self) -> None:
        read_fd, write_fd = os.pipe()
        try:
            os.set_blocking(write_fd, False)
            try:
                while True:
                    os.write(write_fd, b"x" * 4096)
            except BlockingIOError:
                pass
            stop = smoke.StopFlag()
            started = time.monotonic()
            self.assertEqual(smoke._send_control(write_fd, b"touch up 1 1\n", stop, timeout=0.3), "stalled")
            self.assertLess(time.monotonic() - started, 1.0)
            stop(signal.SIGTERM, None)
            self.assertEqual(smoke._send_control(write_fd, b"touch up 1 1\n", stop), "stopped")
            os.read(read_fd, 65536)
            self.assertEqual(smoke._send_control(write_fd, b"touch up 1 1\n", smoke.StopFlag()), "sent")
            os.close(read_fd)
            read_fd = -1
            self.assertEqual(smoke._send_control(write_fd, b"x", smoke.StopFlag()), "closed")
        finally:
            for fd in (read_fd, write_fd):
                if fd >= 0:
                    os.close(fd)


if __name__ == "__main__":
    unittest.main()
