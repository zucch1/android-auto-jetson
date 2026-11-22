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
COLLECTOR = ROOT / "tools" / "perf" / "aa_projection_collect.py"
PERF_FAKE_RECEIVER = ROOT / "tests" / "perf" / "fixtures" / "wired_perf" / "fake_receiver.py"
sys.path.insert(0, str(SMOKE.parent))
import aa_drive_smoke as smoke  # noqa: E402

S = 10**9
ACTIVE = 1_000 * S
LOG_COMPLETE = {"event": "log_complete", "runtime_dropped": 0, "state_overflow": 0,
                "media_rate_limited": 0, "media_backpressure": 0, "shutdown_dropped": 0,
                "max_delivery_lag_ms": 1}


class Run:
    """Writes one runner output directory (receiver.log, display.log, idr-events.jsonl, run.json)."""

    def __init__(self, fps: int = 30, painted_fps: float | None = None, duration: int = 600,
                 width: int = 1280, height: int = 720):
        self.fps, self.duration = fps, duration
        self.first = ACTIVE + 2 * S
        self.receiver = ["Opening in BLOCKING MODE ",
                         {"event": "ready"}, {"event": "active", "emit_ns": ACTIVE},
                         {"event": "video_profile", "width": width, "height": height, "fps": fps,
                          "codec": "h264"}]
        self.display: list = [{"event": "ready"}]
        self.stamped: list = []
        self.run = {"duration_s": duration, "active_ns": ACTIVE,
                    "window_end_ns": self.first + duration * S + S // 2,
                    "receiver_alive_at_end": True, "display_alive_at_end": True,
                    "receiver_exit": 0, "display_exit": 0}
        rate = painted_fps or fps
        count = int((duration + 1) * rate)
        self.paints = [self.first + int(i * S / rate) for i in range(count)]
        # Media plays for the whole drive by default with a constant +12 ms skew.
        self.media_on, self.skew_ms = True, (lambda at_s: 12.0)
        self.audio_from_s, self.audio_until_s = -1.0, None
        # Both writers prove their logs complete unless a case says otherwise.
        self.receiver_tail: list = [LOG_COMPLETE]
        self.display_tail: list = [{"event": "telemetry_complete"}]
        self.drop_audio_at_s: float | None = None
        self.write_sidecar = True

    def freeze(self, at_s: float, seconds: float) -> "Run":
        lo, hi = self.first + int(at_s * S), self.first + int((at_s + seconds) * S)
        self.paints = [t for t in self.paints if not lo <= t < hi]
        return self

    def idr(self, source: str, at_s: float, seconds: float | None, latency_ms: int | None = None,
            delay_ns: int = 0):
        start = self.first + int(at_s * S)
        self.stamped.append({"observed_ns": start + delay_ns, "event": {
            "event": "idr_requested", "source": source, "reason": "waiting_idr", "emit_ns": start}})
        if seconds is not None:
            self.stamped.append({"observed_ns": start + int(seconds * S) + delay_ns, "event": {
                "event": "idr_recovered", "source": source,
                "latency_ms": latency_ms if latency_ms is not None else int(seconds * 1000),
                "episode_requests": 1, "total_requests": 1, "emit_ns": start + int(seconds * S)}})
        return self

    def no_media(self) -> "Run":
        self.media_on = False
        return self

    def skew(self, function) -> "Run":
        """Signed skew (ms) of the frame painted ``at_s`` seconds after first paint."""
        self.skew_ms = function
        return self

    def audio(self, from_s: float, until_s: float | None) -> "Run":
        """Audio slots only between these offsets from first paint."""
        self.audio_from_s, self.audio_until_s = from_s, until_s
        return self

    def write(self, directory: Path) -> Path:
        # Audio playback delay is 10 ms, video delay 10 ms + skew: signed skew = skew.
        display = list(self.display)
        for frame, t in enumerate(self.paints):
            delay = 10_000_000 + int(self.skew_ms((t - self.first) / S) * 1e6)
            display.append({"event": "video_presented", "frame": frame, "scheduled_ns": t - delay,
                            "completed_ns": t, "count": 1})
        receiver = list(self.receiver)
        if self.media_on and self.paints:
            slot = 42_666_666
            start = self.first + int(self.audio_from_s * S)
            stop = (self.first + int(self.audio_until_s * S) if self.audio_until_s is not None
                    else self.paints[-1])
            drop_at = (self.first + int(self.drop_audio_at_s * S)
                       if self.drop_audio_at_s is not None else None)
            while start < stop:
                if drop_at is None or not start <= drop_at < start + slot:
                    receiver.append({"event": "audio_write_completed", "scheduled_ns": start,
                                     "scheduled_end_ns": start + slot, "completed_ns": start,
                                     "presented_ns": start + 10_000_000})
                start += slot
        # The raw logs carry every IDR line (the runner writes each line to its
        # log and, as it arrives, to the sidecar).
        for item in sorted(self.stamped, key=lambda x: x["observed_ns"]):
            event = item["event"]
            (receiver if event.get("source") == "producer" else display).append(event)
        receiver += self.receiver_tail
        display += self.display_tail
        for name, lines in (("receiver.log", receiver), ("display.log", display)):
            (directory / name).write_text("\n".join(
                line if isinstance(line, str) else json.dumps(line, separators=(",", ":"))
                for line in lines) + "\n")
        if self.write_sidecar:
            (directory / "idr-events.jsonl").write_text(
                "".join(json.dumps(x) + "\n" for x in self.stamped))
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
        # Then the 59 windows after the 10 s startup grace clear the 27 fps floor
        # and the signed A/V contract passes with full audio coverage.
        fps = criterion(result, "painted_fps_per_window")
        self.assertEqual(result["verdict"], "PASS", result["criteria"])
        self.assertEqual((fps["windows"], fps["expected_windows"], fps["floor_fps"]), (59, 59, 27.0))
        av = criterion(result, "av_signed_contract")
        self.assertTrue(av["passed"], av)
        self.assertAlmostEqual(av["coverage"], 1.0, places=3)
        self.assertAlmostEqual(av["signed_p50_ms"], 12.0, places=3)

    def test_dropped_audio_slot_cannot_turn_an_av_fail_into_a_pass(self) -> None:
        # Review probe (#90): a 102 ms excursion fails the signed contract with
        # the full stream...
        def spike(at_s: float) -> float:
            return 102.0 if 15.0 <= at_s < 15.04 else 12.0
        full = Run(duration=30).skew(spike)
        self.assertEqual(self.analyze(full)["verdict"], "FAIL")
        self.assertFalse(criterion(self.analyze(full), "av_signed_contract")["passed"])
        # ...and removing only the offending audio slot, with the receiver's
        # real-format media-drop summary, must never read as a PASS.
        probe = Run(duration=30).skew(spike)
        probe.drop_audio_at_s = 15.02
        probe.receiver_tail = [{"event": "media_telemetry_dropped", "rate_limited": 0, "backpressure": 1},
                               dict(LOG_COMPLETE, media_backpressure=1)]
        result = self.analyze(probe)
        self.assertEqual(result["verdict"], "BLOCKED", result["criteria"])
        self.assertIn("av_signed_contract:media_telemetry_lost", result["blocked_on"])

    def test_delayed_active_cannot_clip_an_idr_episode(self) -> None:
        # Review 4 probe (#90): a source-stamped 2 s IDR episode right after
        # first paint FAILs with a prompt activation...
        prompt = Run(duration=30).idr("consumer", 0.0, 2.0)
        self.assertEqual(self.analyze(prompt)["verdict"], "FAIL")
        # ...and the same episode with the `active` line delivered 4 s late
        # (the runner's arrival anchor) is still a 2 s FAIL: the window is
        # anchored at the record's own emit_ns.
        late = Run(duration=30).idr("consumer", 0.0, 2.0)
        late.run["active_ns"] = ACTIVE + 4 * S
        result = self.analyze(late)
        self.assertEqual(result["verdict"], "FAIL", result["criteria"])
        idr = criterion(result, "waiting_idr_max")
        self.assertFalse(idr["passed"])
        self.assertAlmostEqual(idr["longest_s"], 2.0, places=3)

    def test_unstamped_active_is_blocked(self) -> None:
        run = Run(duration=30)
        run.receiver[2] = {"event": "active"}
        result = self.analyze(run)
        self.assertEqual(result["verdict"], "BLOCKED", result["criteria"])
        self.assertIn("waiting_idr_max:active_unstamped", result["blocked_on"])

    def test_missing_idr_sidecar_is_blocked(self) -> None:
        # Review 4 probe (#90): removing only idr-events.jsonl (both IDR events
        # and the completion markers stay in the logs) must never PASS...
        run = Run(duration=30).idr("consumer", 5.0, 0.5)
        run.write_sidecar = False
        result = self.analyze(run)
        self.assertEqual(result["verdict"], "BLOCKED", result["criteria"])
        self.assertIn("waiting_idr_max:idr_sidecar_missing", result["blocked_on"])
        # ...and a missing sidecar cannot hide a 2 s episode the logs show.
        failing = Run(duration=30).idr("consumer", 5.0, 2.0)
        failing.write_sidecar = False
        self.assertNotEqual(self.analyze(failing)["verdict"], "PASS")

    def test_missing_sidecar_keeps_a_confirmed_idr_failure_failed(self) -> None:
        # Review 5 (#90): complete raw logs confirm a 2.5 s recovered episode.
        # The sidecar only adds what the runner saw; its absence cannot shorten
        # an episode both complete logs show, so the failure stays FAIL.
        failing = Run(duration=30).idr("producer", 5.0, 2.5)
        failing.write_sidecar = False
        result = self.analyze(failing)
        self.assertEqual(result["verdict"], "FAIL", result.get("blocked_on"))
        self.assertIs(criterion(result, "waiting_idr_max")["passed"], False)
        self.assertNotIn("inconclusive", criterion(result, "waiting_idr_max"))

    def test_unresolved_wait_with_every_request_failed_fails(self) -> None:
        # The consumer logs the wait transition itself (idr_wait_started); no
        # request ever succeeded and the wait never recovered.
        run = Run(duration=30)
        start = run.first + 10 * S
        run.stamped.append({"observed_ns": start, "event": {
            "event": "idr_wait_started", "source": "consumer", "reason": "decode_awaiting_idr",
            "emit_ns": start}})
        result = self.analyze(run)
        self.assertEqual(result["verdict"], "FAIL", result["criteria"])
        self.assertFalse(criterion(result, "waiting_idr_max")["passed"])

    def test_delayed_idr_records_are_timed_at_emission(self) -> None:
        # Given a 2 s producer episode inside the drive whose two records both
        # reached the runner only after the cutoff (stdout backpressure).
        run = Run(duration=30)
        cutoff_s = 30.5
        run.idr("producer", 26, 2, delay_ns=int((cutoff_s + 3 - 26) * S))
        result = self.analyze(run)
        idr = criterion(result, "waiting_idr_max")
        # Then the episode still counts at its emission times: 2 s > 1 s limit.
        self.assertFalse(idr["passed"], idr)
        self.assertAlmostEqual(idr["longest_s"], 2.0, places=3)
        self.assertEqual(result["verdict"], "FAIL")

    def test_unstamped_idr_records_are_inconclusive(self) -> None:
        run = Run(duration=30)
        run.idr("producer", 10, 0.1)
        for item in run.stamped:
            del item["event"]["emit_ns"]
        result = self.analyze(run)
        self.assertEqual(result["verdict"], "BLOCKED")
        self.assertIn("waiting_idr_max:idr_events_unstamped", result["blocked_on"])

    def test_logs_without_completion_proof_never_pass(self) -> None:
        for tail in ("receiver", "display"):
            with self.subTest(missing=tail):
                run = Run(duration=30)
                setattr(run, tail + "_tail", [])
                self.assertEqual(self.analyze(run)["verdict"], "BLOCKED")

    def test_confirmed_failure_survives_unrelated_loss(self) -> None:
        # Given a painted freeze (display evidence) and lost receiver media.
        run = Run(duration=60).freeze(30, 1.2)
        run.receiver_tail = [dict(LOG_COMPLETE, media_backpressure=3)]
        result = self.analyze(run)
        # Then the confirmed fps failure stays FAIL, not a downgraded BLOCKED.
        self.assertEqual(result["verdict"], "FAIL", result["criteria"])

    def test_lost_receiver_state_records_block_instead_of_judging(self) -> None:
        # Given a run that would PASS, but whose receiver lost state records
        # under a stalled stdout (#90 review: video_profile, idr_*, and
        # session_state_changed feed the criteria).
        run = Run()
        run.receiver.append({"event": "runtime_log_dropped", "backpressure": 4, "state_overflow": 1})
        result = self.analyze(run)
        # Then neither PASS nor FAIL is evidenced: BLOCKED.
        self.assertEqual(result["verdict"], "BLOCKED", result)
        self.assertIn("no_session_drop:receiver_state_log_lost", result["blocked_on"])
        self.assertIn("waiting_idr_max:receiver_state_log_lost", result["blocked_on"])

    def test_droppable_runtime_loss_alone_keeps_the_verdict(self) -> None:
        run = Run()
        run.receiver.append({"event": "runtime_log_dropped", "backpressure": 4, "state_overflow": 0})
        run.receiver_tail = [dict(LOG_COMPLETE, runtime_dropped=4)]
        result = self.analyze(run)
        self.assertEqual(result["verdict"], "PASS", result["criteria"])
        self.assertFalse(result["log_integrity"]["receiver"]["state_lost"])

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

    def av(self, run: Run) -> dict:
        return criterion(self.analyze(run), "av_signed_contract")

    def assert_only_clause_fails(self, av: dict, clause: str) -> None:
        self.assertFalse(av["passed"], av)
        self.assertEqual([k for k, ok in av["clauses"].items() if not ok], [clause], av)

    def test_sustained_offset_fails_the_5_s_mean_in_either_direction(self) -> None:
        # Given 5.5 s of +45 ms (or -45 ms) skew: under 1 % of the pairs, so the
        # tail and the cap pass, but a whole 5 s window averages beyond 40 ms.
        for sign in (1, -1):
            with self.subTest(sign=sign):
                av = self.av(Run().skew(lambda at, s=sign: 45.0 * s if 300 <= at < 305.5 else 12.0))
                self.assert_only_clause_fails(av, "window_mean")
                self.assertAlmostEqual(av["worst_window_mean_ms"], 45.0 * sign, places=3)
        # 39 ms sustained stays inside +/-40 ms.
        self.assertTrue(self.av(Run().skew(lambda at: 39.0 if 300 <= at < 320 else 12.0))["passed"])

    def test_tail_fails_the_p99(self) -> None:
        # Given every 40th frame (2.5 %) 60 ms late: the 5 s means stay near 13 ms.
        frames = Run()
        late = set(frames.paints[::40])
        av = self.av(frames.skew(lambda at: 60.0 if frames.first + int(at * S) in late else 12.0))
        self.assert_only_clause_fails(av, "p99_abs")
        self.assertAlmostEqual(av["abs_p99_ms"], 60.0, places=3)
        self.assertGreater(av["over_50_ms"], 0)

    def test_single_frame_beyond_the_cap_fails_max(self) -> None:
        av = self.av(Run().skew(lambda at: 95.0 if 200.0 <= at < 200.03 else 12.0))
        self.assert_only_clause_fails(av, "max_abs")
        self.assertAlmostEqual(av["abs_max_ms"], 95.0, places=3)
        # The same frame at 85 ms is within the 90 ms cap.
        self.assertTrue(self.av(Run().skew(lambda at: 85.0 if 200.0 <= at < 200.03 else 12.0))["passed"])

    def test_the_drive_smoke_keeps_the_plain_90_ms_cap(self) -> None:
        # The soak's excursion rate (owner decision 2026-11-17) is soak-only: one
        # pair at 90.1 ms still FAILs the 10-minute drive smoke.
        av = self.av(Run().skew(lambda at: 90.1 if 200.0 <= at < 200.03 else 12.0))
        self.assert_only_clause_fails(av, "max_abs")
        self.assertEqual(av["max_clause"], "cap")
        self.assertEqual(av["excursions"], 1)
        self.assertNotIn("excursions", av["clauses"])
        self.assertNotIn("hard_cap", av["clauses"])

    def test_audio_must_cover_90_percent_of_the_drive(self) -> None:
        # No media at all is a FAIL, never n/a.
        none = self.av(Run().no_media())
        self.assertFalse(none["passed"])
        self.assertEqual((none["pairs"], none["coverage"]), (0, 0.0))
        self.assertFalse(none["clauses"]["coverage"])
        # Audio that stops at 243 s (the S24 shape) fails coverage alone.
        short = self.av(Run().audio(-1.0, 243.0))
        self.assert_only_clause_fails(short, "coverage")
        self.assertAlmostEqual(short["coverage"], 243 / 600, places=2)
        # Audio from 50 s to the end covers 91.7 % and passes; from 70 s, 88.3 % fails.
        self.assertTrue(self.av(Run().audio(50.0, None))["passed"])
        self.assert_only_clause_fails(self.av(Run().audio(70.0, None)), "coverage")

    def test_coverage_is_the_union_of_slots_clipped_to_the_drive(self) -> None:
        # Overlapping and out-of-drive slots count once, and only inside the drive.
        slots = [(0, 10), (5, 30), (50, 200)]
        self.assertAlmostEqual(smoke.av_contract.coverage(slots, 20, 120), (10 + 70) / 100)

    def test_window_means_slide_over_every_membership_change(self) -> None:
        # Pairs at 0..9 s, one per second; a 5 s window holds at most 5 of them.
        pairs = [(i * S, float(i)) for i in range(10)]
        low, high = smoke.window_means(pairs, 0, 10 * S, 5)
        self.assertEqual((low, high), (2.0, 7.0))  # [0,5) -> 0..4 ; [5,10) -> 5..9

    def test_window_means_visit_departures(self) -> None:
        # The PR #151 review's counterexample: the full window [1 ns, 5 s + 1 ns)
        # holds only the 80 ms pair, once the pair at 0 has left.
        for sign in (1, -1):
            with self.subTest(sign=sign):
                pairs = [(0, 0.0), (2 * S, 80.0 * sign), (6 * S, 0.0)]
                low, high = smoke.window_means(pairs, 0, 10 * S, 5)
                self.assertEqual(high if sign > 0 else low, 80.0 * sign)
        # Irregular spacing: the lone pair between two departures is still seen.
        pairs = [(0, 10.0), (int(0.7 * S), 10.0), (int(3.1 * S), 90.0), (int(5.9 * S), 10.0)]
        self.assertEqual(smoke.window_means(pairs, 0, 9 * S, 5)[1], 90.0)

    def test_window_bounds_are_half_open_and_full_windows_only(self) -> None:
        # A drive exactly one window long has the single window [0, 5 s): a pair
        # at its start is inside, a pair exactly at 5 s is not.
        self.assertEqual(smoke.window_means([(0, 10.0), (5 * S, 100.0)], 0, 5 * S, 5), (10.0, 10.0))
        # One pair alone is a window mean of its own.
        self.assertEqual(smoke.window_means([(2 * S, -33.0)], 0, 5 * S, 5), (-33.0, -33.0))
        # A pair exactly on the last window's end is outside every full window.
        self.assertIsNone(smoke.window_means([(5 * S, 100.0)], 0, 5 * S, 5))
        # A drive shorter than a window has no full window.
        self.assertIsNone(smoke.window_means([(S, 1.0)], 0, 4 * S, 5))

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
            "episode_requests": 0, "total_requests": 0, "emit_ns": with_latency.first + 3 * S}})
        without.stamped.append({"observed_ns": without.first + 3 * S, "event": {
            "event": "idr_recovered", "source": "consumer", "emit_ns": without.first + 3 * S}})
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
                "event": "idr_requested", "source": "consumer", "reason": "video_consumer_waiting_idr",
                "emit_ns": ACTIVE + int(request_s * S)}})
        run.stamped.append({"observed_ns": ACTIVE + int(recovered_s * S), "event": {
            "event": "idr_recovered", "source": "consumer", **fields,
            "emit_ns": ACTIVE + int(recovered_s * S)}})
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
        run = Run()
        run.receiver.append({"event": "audio_write_completed", "scheduled_ns": run.first,
                             "scheduled_end_ns": run.first + 42_666_666,
                             "completed_ns": run.first, "presented_ns": None})
        av = criterion(self.analyze(run), "av_signed_contract")
        # Then the slot is skipped ("where valid") but reported.
        self.assertTrue(av["passed"])
        self.assertEqual(av["invalid_audio_slots"], 1)

    def test_nonzero_receiver_exit_or_logger_error_is_not_pass(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-exit-") as directory:
            root = Path(directory)
            run = Run()
            run.run["receiver_exit"] = 1
            res = smoke.analyze_dir(run.write(root))
            self.assertNotEqual(res["verdict"], "PASS", res)
            self.assertEqual(res["verdict"], "FAIL")

        with tempfile.TemporaryDirectory(prefix="drive-smoke-logerr-") as directory:
            root = Path(directory)
            run = Run()
            run.receiver.append("stdout_log errno=105")
            res = smoke.analyze_dir(run.write(root))
            self.assertNotEqual(res["verdict"], "PASS", res)
            self.assertEqual(res["verdict"], "FAIL")

    def test_missing_or_null_receiver_exit_is_not_pass(self) -> None:
        # A run.json without a receiver exit code, or with null, cannot show the
        # receiver ended cleanly: analyze(), analyze_dir() and the offline CLI never PASS it.
        for label, mutate in (("missing", lambda r: r.pop("receiver_exit")),
                              ("null", lambda r: r.update(receiver_exit=None))):
            with self.subTest(label), tempfile.TemporaryDirectory(prefix=f"drive-smoke-{label}-") as directory:
                run = Run()
                self.assertEqual(self.analyze(run)["verdict"], "PASS")  # the healthy control
                mutate(run.run)
                res = self.analyze(run)
                self.assertEqual(res["verdict"], "FAIL", res)
                self.assertEqual(res["failure"], "receiver_exit_missing")
                out = run.write(Path(directory))
                self.assertEqual(smoke.analyze_dir(out)["verdict"], "FAIL")

    def test_nonzero_display_exit_is_not_pass(self) -> None:
        # #174: a display that exits non-zero after writing telemetry_complete must FAIL
        # and check_record must refuse the resulting document.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-disp-exit-") as directory:
            root = Path(directory)
            run = Run()
            run.run["display_exit"] = 3
            res = smoke.analyze_dir(run.write(root))
            self.assertEqual(res["verdict"], "FAIL", res)
            self.assertEqual(res["failure"], "display_exit_3")
            gate_doc = record(verdict=res["verdict"])
            self.assertEqual(
                smoke.check_record(gate_doc, receiver_sha256=A, display_sha256=B,
                                   phone_serial=PHONE, now=NOW)[0],
                "drive_smoke_not_passed",
            )

        # A confirmed non-zero display exit overrides a BLOCKED verdict too.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-disp-exit-blocked-") as directory:
            root = Path(directory)
            run = Run()
            run.display_tail = []  # would otherwise be BLOCKED on display_log_incomplete
            run.run["display_exit"] = 3
            res = smoke.analyze_dir(run.write(root))
            self.assertEqual(res["verdict"], "FAIL", res)
            self.assertEqual(res["failure"], "display_exit_3")

    def test_missing_or_null_display_exit_is_not_pass(self) -> None:
        # #174: missing or null display_exit turns PASS into FAIL, but does not override BLOCKED.
        for label, mutate in (("missing", lambda r: r.pop("display_exit")),
                              ("null", lambda r: r.update(display_exit=None))):
            with self.subTest(label), tempfile.TemporaryDirectory(prefix=f"drive-smoke-disp-{label}-") as directory:
                run = Run()
                self.assertEqual(self.analyze(run)["verdict"], "PASS")
                mutate(run.run)
                res = self.analyze(run)
                self.assertEqual(res["verdict"], "FAIL", res)
                self.assertEqual(res["failure"], "display_exit_missing")
                out = run.write(Path(directory))
                self.assertEqual(smoke.analyze_dir(out)["verdict"], "FAIL")

                # Does not override a BLOCKED verdict.
                blocked = Run()
                blocked.display_tail = []
                mutate(blocked.run)
                res_blocked = self.analyze(blocked)
                self.assertEqual(res_blocked["verdict"], "BLOCKED", res_blocked)


NOW = datetime(2026, 11, 16, 12, tzinfo=timezone.utc)
A, B = "a" * 64, "b" * 64


PHONE = "TESTPHONE01"  # synthetic serial; no real phone identifiers in fixtures


def record(finished: datetime = NOW - timedelta(hours=2), **changes) -> dict:
    doc = {"schema": smoke.SCHEMA, "verdict": "PASS", "duration_s": 600, "video_profiles": None,
           "finished_utc": finished.isoformat(), "phone_serial": PHONE,
           "configuration": dict(smoke.DEFAULT_CONFIGURATION),
           "binaries": {"receiver": {"sha256": A}, "display": {"sha256": B}}}
    doc.update(changes)
    return doc


def run_check(path: Path, receiver: str = A, display: str = B, phone: str = PHONE,
              now: str | None = "2026-11-16T12:00:00Z") -> subprocess.CompletedProcess:
    command = [sys.executable, "-B", str(SMOKE), "check", "--record", str(path),
               "--receiver-sha256", receiver, "--display-sha256", display, "--phone-serial", phone]
    return subprocess.run(command + (["--now", now] if now else []),
                          capture_output=True, text=True, check=False)


class GateTests(unittest.TestCase):
    def check(self, doc, receiver=A, display=B, phone=PHONE):
        return smoke.check_record(doc, receiver_sha256=receiver, display_sha256=display,
                                  phone_serial=phone, now=NOW)[0]

    def test_fresh_pass_for_the_same_bytes_admits_the_soak(self) -> None:
        self.assertIsNone(self.check(record()))

    def test_refusals_are_named(self) -> None:
        cases = {
            "drive_smoke_missing": [None, {"schema": "other"},
                                    record(schema="aa-drive-smoke/1")],  # pre-contract records
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

    def test_only_the_soak_phone_is_accepted(self) -> None:
        # The A26 smoke must not unlock a soak on another phone, and vice versa.
        self.assertEqual(self.check(record(), phone="OTHERPHONE9"), "drive_smoke_phone_mismatch")
        # A serial the launcher could not verify on the soak's port blocks too.
        for phone in ("", None, "bad serial;id"):
            with self.subTest(phone=phone):
                self.assertEqual(self.check(record(), phone=phone), "drive_smoke_phone_unverified")

    def test_only_the_default_configuration_is_accepted(self) -> None:
        for change in ({"display_flags": ["--presentation", "gpu"], "presentation": "gpu"},
                       {"display_flags": ["--decoder", "hardware"], "display_decoder": "hardware"},
                       {"receiver_flags": ["--idr-recovery", "nudge"]}):
            with self.subTest(change=change):
                configuration = {**smoke.DEFAULT_CONFIGURATION, **change}
                self.assertEqual(self.check(record(configuration=configuration)),
                                 "drive_smoke_configuration")

    def test_missing_phone_or_configuration_fails_closed(self) -> None:
        for field in ("phone_serial", "configuration"):
            with self.subTest(field=field):
                doc = record()
                del doc[field]
                self.assertEqual(self.check(doc), "drive_smoke_malformed")
        partial = dict(smoke.DEFAULT_CONFIGURATION)
        del partial["presentation"]
        for doc in (record(phone_serial=""), record(phone_serial=7), record(configuration=[]),
                    record(configuration=partial)):
            with self.subTest(doc=doc):
                self.assertEqual(self.check(doc), "drive_smoke_malformed")

    def test_configuration_reads_the_launched_commands(self) -> None:
        receiver = ["aa-wired-receiver", "--port", "1-2.1", "--store", "s.json",
                    "--require-video-consumer", "--video-socket", "v", "--input-socket", "i",
                    "--duration-ms", "1"]
        display = ["aa-dummy-display", "--test-only-autostart", "--test-only-control-stdin",
                   "--video-socket", "v", "--input-socket", "i"]
        self.assertEqual(smoke.configuration(receiver, display), smoke.DEFAULT_CONFIGURATION)
        tuned = smoke.configuration(receiver + ["--decoder", "hardware"],
                                    display + ["--presentation", "zerocopy", "--decoder", "auto"])
        self.assertEqual((tuned["receiver_decoder"], tuned["display_decoder"], tuned["presentation"]),
                         ("hardware", "auto", "zerocopy"))
        self.assertEqual(tuned["display_flags"], ["--presentation", "zerocopy", "--decoder", "auto"])

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

    def launch(self, root: Path, *extra: str, fail_display_hash: bool = False,
               swapped: bool = False, phone_setup_fails: bool = False,
               adb_line: str = f"{PHONE}              device usb:1-2.1 product:x model:FAKE",
               phone: str | None = PHONE) -> tuple[subprocess.CompletedProcess, str]:
        stub_dir = root / "bin"
        stub_dir.mkdir(exist_ok=True)
        log = root / "ssh.log"
        log.write_text("")
        (stub_dir / "ssh").write_text(f'''#!/usr/bin/env bash
printf '%s\\n' "$*" >> {str(log)!r}
case "$*" in
    *"bash -s -- "*) eval "${{@: -1}}"; exit $? ;;  # the remote stop, run on this "target"
    *"python3 -"*) if [ -n "${{STUB_COLLECTOR_SWAPPED:-}}" ]; then
                       printf '%s\\n' "$*" | grep -q -- "--phone-serial '{PHONE}'" || exit 1
                       echo "error: phone_identity_mismatch: another phone on --port" >&2
                       exit 2
                   fi
                   if [ -n "${{STUB_COLLECTOR_PHONE_SETUP:-}}" ]; then
                       echo "error: phone_setup_failed: maps: Error: Activity not started" >&2
                       exit 2
                   fi
                   exit 1 ;;
    *sha256sum*aa-wired-receiver*) printf '%s  r\\n' {A}; exit 0 ;;
    *sha256sum*aa-dummy-display*) [ -n "${{FAIL_DISPLAY_HASH:-}}" ] && exit 1
                                  printf '%s  d\\n' {B}; exit 0 ;;
    *"adb devices -l"*) printf 'List of devices attached\\n%s\\n' "${{STUB_ADB_LINE:-}}"; exit 0 ;;
    *xrandr* | *drm*) printf 'connected\\n'; exit 0 ;;
    *pactl* | *pw-cli*) printf '0\\tfake\\tPipeWire\\n'; exit 0 ;;
esac
exit 0
''')
        (stub_dir / "ssh").chmod(0o755)
        env = {"PATH": f"{stub_dir}:/usr/bin:/bin", "AA_PERF_HOST": "fake.local", "AA_DISPLAY": ":0",
               "STUB_ADB_LINE": adb_line}
        if fail_display_hash:
            env["FAIL_DISPLAY_HASH"] = "1"
        if swapped:
            env["STUB_COLLECTOR_SWAPPED"] = "1"
        if phone_setup_fails:
            env["STUB_COLLECTOR_PHONE_SETUP"] = "1"
        process = subprocess.run(
            ["bash", str(LAUNCHER), "--mode", "wired", "--duration", "1800s",
             "--out", str(root / "out.json"), "--media-uri", "file:///sdcard/Music/fixture.opus",
             "--media-component", "org.example.player/.Play",
             *(["--phone-serial", phone] if phone is not None else []), *extra],
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

    def test_soak_phone_must_be_the_smoke_phone_and_on_the_port(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            good = self.write(root, "good.json", record(datetime.now(timezone.utc)))
            # Given a smoke from another phone than the soak's.
            other = self.write(root, "other.json",
                               record(datetime.now(timezone.utc), phone_serial="OTHERPHONE9"))
            self.assert_blocked(root, "drive_smoke_phone_mismatch", "--drive-smoke", other)
            # Given the soak's serial not attached, attached on another port, offline, or absent.
            for adb_line in ("", f"{PHONE} device usb:1-2.3 model:FAKE",
                             f"{PHONE} offline usb:1-2.1"):
                with self.subTest(adb_line=adb_line):
                    self.assert_blocked(root, "drive_smoke_phone_unverified", "--drive-smoke", good,
                                        adb_line=adb_line)
            self.assert_blocked(root, "drive_smoke_phone_unverified", "--drive-smoke", good,
                                phone=None)

    def test_swap_after_the_check_is_caught_where_the_port_is_used(self) -> None:
        # Given the adb check passing for the soak phone, and then another phone
        # on the port when the collector's receiver opens its link (emulated by
        # the stub collector, which only reports it when it was handed the serial).
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            good = self.write(root, "good.json", record(datetime.now(timezone.utc)))
            process, ssh = self.launch(root, "--drive-smoke", good, swapped=True)
            # Then the verified serial was carried into the collector, and the
            # stage is BLOCKED with its own reason.
            self.assertEqual(process.returncode, 3, process.stderr)
            self.assertIn(f"--phone-serial '{PHONE}'", ssh)
            self.assertEqual(json.loads((root / "out.json").read_text())["blocked_on"],
                             "phone_identity_mismatch", process.stderr)

    def test_a_failed_phone_setup_blocks_with_its_own_reason(self) -> None:
        # Given a collector whose required adb setup (here the Maps launch) failed.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            good = self.write(root, "good.json", record(datetime.now(timezone.utc)))
            process, _ = self.launch(root, "--drive-smoke", good, phone_setup_fails=True)
            # Then the stage is BLOCKED phone_setup_failed, never collector_failed or a PASS.
            self.assertEqual(process.returncode, 3, process.stderr)
            self.assertEqual(json.loads((root / "out.json").read_text())["blocked_on"],
                             "phone_setup_failed", process.stderr)

    def test_matching_smoke_reaches_the_collector(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            good = self.write(root, "good.json", record(datetime.now(timezone.utc)))
            process, ssh = self.launch(root, "--drive-smoke", good)
            # The stub collector fails, so the stage is BLOCKED on that, not on the gate.
            self.assertEqual(json.loads((root / "out.json").read_text())["blocked_on"],
                             "collector_failed", process.stderr)
            self.assertIn("python3 -", ssh)
            # The stage drives the moving-map workload with its media (#137).
            self.assertIn("--workload moving-map --media-uri 'file:///sdcard/Music/fixture.opus'", ssh)

    def test_a_live_stage_needs_media_for_the_moving_map_workload(self) -> None:
        with tempfile.TemporaryDirectory(prefix="drive-smoke-launcher-") as directory:
            root = Path(directory)
            for extra in ((), ("--media-uri", "file:///x'y")):
                process = subprocess.run(
                    ["bash", str(LAUNCHER), "--mode", "wired", "--duration", "1800s",
                     "--out", str(root / "out.json"), *extra],
                    capture_output=True, text=True, timeout=60, check=False,
                    env={"PATH": "/usr/bin:/bin", "AA_PERF_HOST": "fake.local"})
                self.assertEqual(process.returncode, 2, process.stderr)
                self.assertIn("usage error", process.stderr)
                self.assertFalse((root / "out.json").exists())



class CollectorPhoneBindingTests(unittest.TestCase):
    """The soak collector pins its receiver to the gate-verified phone (#137)."""

    def collect(self, root: Path, *extra: str, **env: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            [sys.executable, "-B", str(COLLECTOR), "--receiver", str(PERF_FAKE_RECEIVER),
             "--port", "1-2.1", "--duration-seconds", "3", "--interval", "1",
             "--startup-timeout-seconds", "2", "--no-display", "--state-dir", str(root / "state"),
             "--allow-unoptimized-build", *extra],
            capture_output=True, text=True, timeout=120, check=False, env={**os.environ, **env})

    def test_same_phone_collects(self) -> None:
        with tempfile.TemporaryDirectory(prefix="collector-phone-") as directory:
            result = self.collect(Path(directory), "--phone-serial", PHONE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["schema"], "aa-projection-perf-capture/3")

    def test_other_phone_at_the_first_link_is_refused(self) -> None:
        with tempfile.TemporaryDirectory(prefix="collector-phone-") as directory:
            result = self.collect(Path(directory), "--phone-serial", PHONE,
                                  FAKE_RECEIVER_ATTACHED_SERIAL="OTHERPHONE9")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("error: phone_identity_mismatch:", result.stderr)
        self.assertEqual(result.stdout, "", "no capture for a run on another phone")

    def test_swap_mid_stage_is_refused(self) -> None:
        # Given the soak phone at the start and another phone at a reconnect.
        with tempfile.TemporaryDirectory(prefix="collector-phone-") as directory:
            result = self.collect(Path(directory), "--phone-serial", PHONE,
                                  FAKE_RECEIVER_SWAP_AFTER_MS="1500")
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("error: phone_identity_mismatch:", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_without_a_serial_the_receiver_is_not_pinned(self) -> None:
        # (The live launcher always passes one; offline/tooling runs need not.)
        with tempfile.TemporaryDirectory(prefix="collector-phone-") as directory:
            result = self.collect(Path(directory), FAKE_RECEIVER_ATTACHED_SERIAL="OTHERPHONE9")
        self.assertEqual(result.returncode, 0, result.stderr)

FAKE_RECEIVER = """#!/usr/bin/env python3
import os, signal, subprocess, sys, time
directory = os.environ["FAKE_DIR"]
overflow_mode = os.environ.get("FAKE_RECEIVER_OVERFLOW", "")
state = {"stopping": False}
def stop(*_):
    state["stopping"] = True
    if overflow_mode == "stderr_zero":
        print("stdout_log errno=105", flush=True)
        sys.exit(0)
    if overflow_mode:
        # A latched logger error never writes log_complete and exits nonzero.
        sys.exit(1)
    # Like the real receiver: a clean stop drains the log and exits 0.
    print('{"event":"stopping","duration_ms":0}', flush=True)
    print('{"event":"log_complete","runtime_dropped":0,"state_overflow":0,"media_rate_limited":0,'
          '"media_backpressure":0,"shutdown_dropped":0,"max_delivery_lag_ms":0}', flush=True)
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
grandchild = subprocess.Popen(["sleep", "300"])
with open(os.path.join(directory, "receiver.pids"), "w") as handle:
    handle.write(f"{os.getpid()} {grandchild.pid}")
print('{"event":"ready"}', flush=True)
time.sleep(0.5)
if os.environ.get("FAKE_RECEIVER_HEALTHY_STREAM"):
    # Like the real receiver (#90): the activation anchor carries its emission time.
    print('{"event":"active","emit_ns":%d}' % time.monotonic_ns(), flush=True)
else:
    print('{"event":"active"}', flush=True)
print('{"event":"video_profile","width":1280,"height":720,"fps":30,"codec":"h264"}', flush=True)
if os.environ.get("FAKE_RECEIVER_HEALTHY_STREAM"):
    start = time.monotonic_ns()
    slot = 40_000_000
    next_slot = start - 1_000_000_000
    overflow_at = start + int(float(os.environ.get("FAKE_RECEIVER_OVERFLOW_AT_S", "6.4")) * 1e9)
    while not state["stopping"]:
        now = time.monotonic_ns()
        while next_slot < now + 200_000_000:
            print('{"event":"audio_write_completed","scheduled_ns":%d,"scheduled_end_ns":%d,'
                  '"completed_ns":%d,"presented_ns":%d}'
                  % (next_slot, next_slot + slot, next_slot, next_slot + 10_000_000), flush=True)
            next_slot += slot
        if overflow_mode and now >= overflow_at:
            # Overflow submit_pending near cutoff: idr_* records are lost, g_running becomes false,
            # and teardown completes shortly after the cutoff exiting 1 (or stderr errno=105).
            if overflow_mode == "exit_before_stop":
                time.sleep(0.4)
                sys.exit(1)
            if overflow_mode == "stderr_zero":
                print("stdout_log errno=105", flush=True)
                time.sleep(0.5)
                sys.exit(0)
            time.sleep(0.5)
            sys.exit(1)
        time.sleep(0.05)
time.sleep(300)
"""

FAKE_DISPLAY = """#!/usr/bin/env python3
import os, signal, sys, threading, time
with open(os.path.join(os.environ["FAKE_DIR"], "display.pids"), "w") as handle:
    handle.write(str(os.getpid()))
if os.environ.get("FAKE_DISPLAY_HEALTHY_STREAM"):
    # Like the real display (#90): a runner stop writes the completion record last.
    def stop(*_):
        print('{"event":"telemetry_complete"}', flush=True)
        os._exit(0)
    signal.signal(signal.SIGTERM, stop)
    time.sleep(0.55)
t0 = time.monotonic_ns()
print('{"event":"video_presented","frame":0,"scheduled_ns":%d,"completed_ns":%d,"count":1}'
      % (t0 - 22_000_000, t0), flush=True)
if os.environ.get("FAKE_DISPLAY_HEALTHY_STREAM"):
    def drain():
        for _ in sys.stdin:
            pass
    threading.Thread(target=drain, daemon=True).start()
    frame = 1
    end_ns = t0 + 8 * 10**9
    while time.monotonic_ns() < end_ns:
        target = t0 + (frame * 10**9) // 30
        now = time.monotonic_ns()
        if target > now:
            time.sleep((target - now) / 1e9)
        print('{"event":"video_presented","frame":%d,"scheduled_ns":%d,"completed_ns":%d,"count":1}'
              % (frame, target - 22_000_000, target), flush=True)
        frame += 1
elif not os.environ.get("FAKE_DISPLAY_STALL"):
    for _ in sys.stdin:
        pass
time.sleep(300)
"""

# The fake phone: media player state, pidof, USB re-enumeration after setFunctions.
FAKE_ADB = (ROOT / "tests/perf/fixtures/wired_perf/fake_phone_adb.sh").read_text()

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
elif hook == "passing":
    # The drive's own analysis passes, so only the cleanup can stop a PASS.
    aa_drive_smoke.analyze_dir = lambda out: {"verdict": "PASS", "criteria": []}
elif hook == "overflow":
    # Exercise real analyze_dir() over short 1s windows so 6s has 6 full windows.
    aa_drive_smoke.WINDOW_S = 1
    aa_drive_smoke.FPS_STARTUP_GRACE_S = 0
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

    def start(self, root: Path, hook: str = "", *extra: str,
              component: str | None = "org.example.player/.Play", **env_extra: str) -> subprocess.Popen:
        build, bin_dir = root / "build", root / "bin"
        build.mkdir()
        bin_dir.mkdir()
        (build / "CMakeCache.txt").write_text("CMAKE_BUILD_TYPE:STRING=Release\n")
        for path, body in ((build / "aa-wired-receiver", FAKE_RECEIVER),
                           (build / "aa-dummy-display", FAKE_DISPLAY), (bin_dir / "adb", FAKE_ADB)):
            path.write_text(body)
            path.chmod(0o755)
        (root / "store.json").write_text("{}")
        env = dict(os.environ, FAKE_DIR=str(root), DRIVE_HOOK=hook, PATH=f"{bin_dir}:/usr/bin:/bin",
                   FAKE_ADB_LOG=str(root / "adb.log"))
        if hook == "stall":
            env["FAKE_DISPLAY_STALL"] = "1"
        env.update(env_extra)
        if component is not None:
            extra = ("--media-component", component, *extra)
        return subprocess.Popen(
            [sys.executable, "-B", "-c", DRIVER, str(SMOKE.parent), "run",
             "--serial", "FAKESERIAL", "--port", "9-9", "--build-dir", str(build),
             "--store", str(root / "store.json"), "--display-env", ":99",
             "--media-uri", "file:///sdcard/Music/fake.opus", *extra,
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
        # The player was stopped and verified before the USB switch.
        self.assertEqual(result["cleanup_failures"], [], adb)

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

    def test_cleanup_stops_and_verifies_the_player_before_the_usb_switch(self) -> None:
        # Given a drive with an explicit media component (the player is known).
        with tempfile.TemporaryDirectory(prefix="drive-smoke-cleanup-") as directory:
            root = Path(directory)
            runner = self.start(root, "", "--duration", "3")
            try:
                output, _ = runner.communicate(timeout=90)
            finally:
                if runner.poll() is None:
                    runner.kill()
                if (root / "receiver.pids").exists():
                    for pid in self.pids(root):
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
            adb = (root / "adb.log").read_text().splitlines()
            result = json.loads((root / "out" / "result.json").read_text())
            running = (root / "adb.log.phone" / "running.org.example.player").exists()
        # Then the player was force-stopped and verified gone, then USB switched.
        self.assertEqual(adb[-3:], ["-s FAKESERIAL shell am force-stop org.example.player",
                                    "-s FAKESERIAL shell pidof org.example.player",
                                    "-s FAKESERIAL shell svc usb setFunctions mtp"], output)
        self.assertFalse(running, adb)
        self.assertEqual(result["cleanup_failures"], [], output)

    def run_to_end(self, root: Path, *args, **kwargs) -> tuple[subprocess.CompletedProcess, str]:
        runner = self.start(root, *args, **kwargs)
        try:
            output, _ = runner.communicate(timeout=90)
        finally:
            if runner.poll() is None:
                runner.kill()
            if (root / "receiver.pids").exists():
                for pid in self.pids(root):
                    try:
                        os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
        return runner, output

    def test_an_omitted_media_component_is_refused_before_the_phone_is_touched(self) -> None:
        for component in (None, "org.example.player", "player/.Play", "a/b/c"):
            with self.subTest(component=component), \
                    tempfile.TemporaryDirectory(prefix="drive-smoke-component-") as directory:
                root = Path(directory)
                runner, output = self.run_to_end(root, "", "--duration", "3", component=component)
                self.assertEqual(runner.returncode, 2, output)
                self.assertFalse((root / "adb.log").exists(), "the phone was touched")
                self.assertFalse((root / "out" / "result.json").exists())

    def test_a_player_that_is_not_shown_stopped_blocks_a_passing_drive(self) -> None:
        # Given a drive whose analysis passes, and a player that never stops.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-stuck-") as directory:
            root = Path(directory)
            runner, output = self.run_to_end(root, "passing", "--duration", "3",
                                             FAKE_ADB_STUCK="org.example.player")
            result = json.loads((root / "out" / "result.json").read_text())
        # Then the drive is BLOCKED by name (exit 3), so the soak gate refuses it.
        self.assertEqual(runner.returncode, 3, output)
        self.assertEqual((result["verdict"], result["blocked_on"]),
                         ("BLOCKED", "cleanup:media_not_stopped"), output)
        self.assertTrue(result["cleanup_failures"][0].startswith("media_stop_unverified"))
        self.assertEqual(smoke.check_record(result, receiver_sha256=A, display_sha256=B,
                                            phone_serial="FAKESERIAL", now=NOW)[0] is None, False)
        # And the same drive with a player that does stop passes.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-stuck-") as directory:
            runner, output = self.run_to_end(Path(directory), "passing", "--duration", "3")
        self.assertEqual(runner.returncode, 0, output)

    def test_media_is_relaunched_when_audio_stops(self) -> None:
        # Given a 12 s drive whose (fake) receiver never logs an audio write.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-media-") as directory:
            root = Path(directory)
            runner = self.start(root, "", "--duration", "12")
            try:
                output, _ = runner.communicate(timeout=90)
            finally:
                if runner.poll() is None:
                    runner.kill()
                if (root / "receiver.pids").exists():
                    for pid in self.pids(root):
                        try:
                            os.kill(pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
            # Then the media intent was re-sent during the drive, Maps brought back
            # each time, and the run still fails on coverage (no audio arrived).
            adb = (root / "adb.log").read_text().splitlines()
            media = [line for line in adb if "-d file:///sdcard/Music/fake.opus" in line]
            maps = [line for line in adb if "com.google.android.apps.maps" in line]
            result = json.loads((root / "out" / "result.json").read_text())
            self.assertGreaterEqual(result["media_relaunches"], 1, output)
            self.assertEqual(len(media), 1 + result["media_relaunches"], adb)
            self.assertEqual(len(maps), 1 + result["media_relaunches"], adb)
            self.assertFalse(criterion(result, "av_signed_contract")["clauses"]["coverage"])

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

    def test_logger_overflow_near_cutoff_never_passes(self) -> None:
        # Control: a healthy 6s stream passes end-to-end when the receiver exits 0.
        with tempfile.TemporaryDirectory(prefix="drive-smoke-healthy-") as directory:
            root = Path(directory)
            runner, output = self.run_to_end(
                root, "overflow", "--duration", "6",
                FAKE_RECEIVER_HEALTHY_STREAM="1", FAKE_DISPLAY_HEALTHY_STREAM="1")
            result = json.loads((root / "out" / "result.json").read_text())
            self.assertEqual(runner.returncode, 0, output)
            self.assertEqual(result["verdict"], "PASS", result)

        # Regression: submit_pending overflows near the cutoff (IDR events lost,
        # receiver still alive at window_end_ns, then exits 1 or logs stdout_log errno=105).
        for mode in ("1", "stderr_zero"):
            with self.subTest(mode=mode), \
                    tempfile.TemporaryDirectory(prefix="drive-smoke-overflow-") as directory:
                root = Path(directory)
                runner, output = self.run_to_end(
                    root, "overflow", "--duration", "6",
                    FAKE_RECEIVER_HEALTHY_STREAM="1", FAKE_DISPLAY_HEALTHY_STREAM="1",
                    FAKE_RECEIVER_OVERFLOW=mode)
                run_doc = json.loads((root / "out" / "run.json").read_text())
                result = json.loads((root / "out" / "result.json").read_text())
                self.assertTrue(run_doc["receiver_alive_at_end"], run_doc)
                self.assertNotEqual(result["verdict"], "PASS", result)
                self.assertNotEqual(runner.returncode, 0, output)
                self.assertEqual(result["verdict"], "FAIL", result)
                self.assertEqual(runner.returncode, 1, output)


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
