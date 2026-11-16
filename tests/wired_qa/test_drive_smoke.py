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
import subprocess
import sys
import tempfile
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


NOW = datetime(2026, 11, 16, 12, tzinfo=timezone.utc)


def record(**changes) -> dict:
    doc = {"schema": smoke.SCHEMA, "verdict": "PASS", "duration_s": 600, "video_profiles": None,
           "finished_utc": (NOW - timedelta(hours=2)).isoformat(),
           "binaries": {"receiver": {"sha256": "a" * 64}, "display": {"sha256": "b" * 64}}}
    doc.update(changes)
    return doc


class GateTests(unittest.TestCase):
    def check(self, doc, receiver="a" * 64, display="b" * 64):
        return smoke.check_record(doc, receiver_sha256=receiver, display_sha256=display, now=NOW)[0]

    def test_fresh_pass_for_the_same_bytes_admits_the_soak(self) -> None:
        self.assertIsNone(self.check(record()))

    def test_refusals_are_named(self) -> None:
        cases = {
            "drive_smoke_missing": [None, {"schema": "other"}, record(finished_utc="2026-11-16")],
            "drive_smoke_not_passed": [record(verdict="FAIL"), record(verdict="BLOCKED")],
            "drive_smoke_stale": [record(finished_utc=(NOW - timedelta(hours=25)).isoformat()),
                                  record(finished_utc=(NOW + timedelta(hours=1)).isoformat())],
            "drive_smoke_profile": [record(video_profiles="1920x1080@60:h264")],
            "drive_smoke_short": [record(duration_s=120)],
        }
        for reason, docs in cases.items():
            for doc in docs:
                with self.subTest(reason=reason, doc=doc):
                    self.assertEqual(self.check(doc), reason)
        self.assertEqual(self.check(record(), receiver="c" * 64), "drive_smoke_build_mismatch")
        self.assertEqual(self.check(record(), display="c" * 64), "drive_smoke_build_mismatch")

    def test_cli_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-gate-") as directory:
            path = Path(directory) / "result.json"
            path.write_text(json.dumps(record()))
            base = [sys.executable, "-B", str(SMOKE), "check", "--record", str(path),
                    "--display-sha256", "b" * 64, "--now", "2026-11-16T12:00:00Z"]
            ok = subprocess.run(base + ["--receiver-sha256", "a" * 64], capture_output=True, text=True)
            bad = subprocess.run(base + ["--receiver-sha256", "c" * 64], capture_output=True, text=True)
        self.assertEqual(ok.returncode, 0, ok.stderr)
        self.assertEqual((bad.returncode, bad.stdout.strip()), (3, "drive_smoke_build_mismatch"))


class LauncherGateTests(unittest.TestCase):
    """The perf launcher refuses every live stage before the collector starts."""

    def launch(self, root: Path, *extra: str) -> tuple[subprocess.CompletedProcess, str]:
        stub_dir = root / "bin"
        stub_dir.mkdir(exist_ok=True)
        log = root / "ssh.log"
        log.write_text("")
        (stub_dir / "ssh").write_text(f'''#!/usr/bin/env bash
printf '%s\\n' "$*" >> {str(log)!r}
case "$*" in
    *"python3 -"*) exit 1 ;;
    *sha256sum*) printf '%s  r\\n%s  d\\n' {"a" * 64} {"b" * 64}; exit 0 ;;
    *xrandr* | *drm*) printf 'connected\\n'; exit 0 ;;
    *pactl* | *pw-cli*) printf '0\\tfake\\tPipeWire\\n'; exit 0 ;;
esac
exit 0
''')
        (stub_dir / "ssh").chmod(0o755)
        process = subprocess.run(
            ["bash", str(LAUNCHER), "--mode", "wired", "--duration", "60s",
             "--out", str(root / "out.json"), "--now", "2026-11-16T12:00:00Z", *extra],
            capture_output=True, text=True, timeout=120, check=False,
            env={"PATH": f"{stub_dir}:/usr/bin:/bin", "AA_PERF_HOST": "fake.local", "AA_DISPLAY": ":0"})
        return process, log.read_text()

    def test_no_or_wrong_smoke_blocks_before_the_collector(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            mismatch = root / "mismatch.json"
            mismatch.write_text(json.dumps(record(binaries={"receiver": {"sha256": "c" * 64}})))
            for extra, reason in (((), "drive_smoke_missing"),
                                  (("--drive-smoke", str(mismatch)), "drive_smoke_build_mismatch")):
                with self.subTest(reason=reason):
                    process, ssh = self.launch(root, *extra)
                    self.assertEqual(process.returncode, 3, process.stderr)
                    self.assertEqual(json.loads((root / "out.json").read_text())["blocked_on"], reason)
                    self.assertNotIn("python3 -", ssh, "the stage started without a drive smoke")

    def test_matching_smoke_reaches_the_collector(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            good = root / "good.json"
            good.write_text(json.dumps(record()))
            process, ssh = self.launch(root, "--drive-smoke", str(good))
            # The stub collector fails, so the stage is BLOCKED on that, not on the gate.
            self.assertEqual(json.loads((root / "out.json").read_text())["blocked_on"],
                             "collector_failed", process.stderr)
            self.assertIn("python3 -", ssh)


if __name__ == "__main__":
    unittest.main()
