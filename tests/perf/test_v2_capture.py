#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Focused active-window endpoint capture regressions; no hardware required."""
from __future__ import annotations

import argparse
from dataclasses import replace
import json
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/perf"))
from aa_video_measurements import DecodeEndpoint, IpcEndpoint, LIMIT, VideoMeasurements
from aa_measurement_coverage import (CaptureEvidence, Sample, ThermalZone, Window,
                                     coverage_gaps, parse_capture, reduce_capture)
import aa_projection_collect as collector
from test_continuity_regressions import Process

SECOND = 1_000_000_000


def receive(frame: int, stamp: int) -> IpcEndpoint:
    return IpcEndpoint((7, 8, frame, frame, 999), 0, stamp, 1024)


def decoded(frame: int, stamp: int) -> DecodeEndpoint:
    return DecodeEndpoint((0, frame, frame, 999), stamp - 1, stamp)


def test_startup_and_shutdown_cannot_contribute() -> None:
    # Given startup evidence, including an exact-S endpoint.
    video = VideoMeasurements()
    video.decode(decoded(1, SECOND))
    video.decode(DecodeEndpoint((0, 2, 2, 999), 2 * SECOND, 2 * SECOND))
    # When the active window starts and freezes before shutdown ingestion.
    video.start_window(0, 2 * SECOND)
    video.decode(decoded(3, 3 * SECOND))
    video.advance(4 * SECOND)
    evidence = video.finish(4 * SECOND)
    video.decode(decoded(4, 4 * SECOND))
    # Then startup is discarded, exact-S retained, and the frozen ring is immutable.
    assert sum(block.decode.count for block in evidence.blocks) == 2
    assert evidence.decode_latency_ns == (0, 1)
    assert tuple(video.decode_samples) == (0, 1)


def test_late_replay_cannot_heal_a_sealed_interval() -> None:
    # Given an empty first interval sealed at the ten-second horizon.
    video = VideoMeasurements()
    video.start_window(0, 0)
    for stamp in (5, 10, 15):
        video.advance(stamp * SECOND)
    # When old evidence is replayed and timely open evidence arrives out of order.
    video.decode(decoded(1, 5 * SECOND))
    video.decode(decoded(2, 12 * SECOND))
    video.decode(decoded(3, 8 * SECOND))
    evidence = video.finish(15 * SECOND)
    # Then only open intervals count, using completion times rather than arrival.
    assert [block.decode.count for block in evidence.blocks] == [0, 1, 1]
    assert evidence.decode_latency_ns == (1, 1)
    assert evidence.blocks[1].decode.first_ns == 8 * SECOND


def test_sealed_dedup_survives_more_than_4096_total_observations() -> None:
    # Given many observations spread over sealed sample intervals.
    video = VideoMeasurements()
    video.start_window(0, 0)
    total = LIMIT + 400
    for frame in range(1, total + 1):
        stamp = frame * SECOND
        video.ipc("send", receive(frame, stamp - frame))
        video.ipc("receive", receive(frame, stamp))
        video.decode(DecodeEndpoint((0, frame, frame, 999), stamp - frame, stamp))
        video.advance(stamp)
    # When an evicted sealed identity is replayed before finalization.
    video.ipc("receive", receive(1, SECOND))
    video.decode(decoded(1, SECOND))
    evidence = video.finish(total * SECOND)
    # Then whole-run coverage survives ring eviction without recounting.
    assert sum(block.decode.count for block in evidence.blocks) == total
    assert sum(block.ipc_consumption.bytes for block in evidence.blocks) == total * 1024
    assert sum(block.ipc_matches.count for block in evidence.blocks) == total
    assert len(evidence.ipc_latency_ns) == LIMIT
    assert len(evidence.decode_latency_ns) == LIMIT
    assert evidence.decode_latency_ns == tuple(range(total - LIMIT + 1, total + 1))
    assert evidence.ipc_latency_ns == evidence.decode_latency_ns
    assert len(video.decoded) <= LIMIT and len(video.receive_seen) <= LIMIT
    assert not video.issues


def test_open_capacity_loss_is_latched_without_recounting() -> None:
    # Given a single open interval containing more than the bounded capacity.
    video = VideoMeasurements()
    video.start_window(0, 0)
    for frame in range(LIMIT + 1):
        video.decode(decoded(frame, SECOND + frame))
    # When an old identity is replayed after overflow.
    video.decode(decoded(0, SECOND))
    video.advance(2 * SECOND)
    evidence = video.finish(2 * SECOND)
    # Then admission never evicts a required open dedup key silently.
    assert sum(block.decode.count for block in evidence.blocks) == LIMIT
    assert "coverage_capacity" in video.issues


def test_pre_start_send_does_not_join_an_active_receive() -> None:
    # Given a send before S and a receive after S.
    video = VideoMeasurements()
    video.ipc("send", receive(1, SECOND))
    video.start_window(0, 2 * SECOND)
    # When consumption is observed without an eligible send.
    video.ipc("receive", receive(1, 3 * SECOND))
    video.advance(4 * SECOND)
    evidence = video.finish(4 * SECOND)
    # Then consumption remains real but latency is absent.
    assert evidence.blocks[0].ipc_consumption.count == 1
    assert evidence.blocks[0].ipc_consumption.begin_ns == 2 * SECOND
    assert evidence.blocks[0].ipc_matches.count == 0
    assert evidence.ipc_latency_ns == ()


def test_endpoint_window_edges_pass_at_ten_seconds_but_not_above() -> None:
    # Given identical completion timestamps with exact and over-limit trailing edges.
    for extra in (0, 1):
        video = VideoMeasurements()
        video.start_window(0, 0)
        video.ipc("send", receive(1, 10 * SECOND - 1))
        video.ipc("receive", receive(1, 10 * SECOND))
        video.decode(decoded(1, 10 * SECOND))
        stamps = (0, 5 * SECOND, 10 * SECOND, 15 * SECOND, 20 * SECOND + extra)
        samples = tuple(Sample(t, 100.0, t // SECOND, (ThermalZone("zone:CPU", -2.0),),
                               True, 1, 1, 1, 1, 1) for t in stamps)
        for stamp in stamps[1:]:
            video.advance(stamp)
        # When captured blocks are independently reduced over their actual window.
        reduction = reduce_capture(CaptureEvidence(0, 100, samples, video.finish(stamps[-1])),
                                   Window(0, stamps[-1]))
        gaps = coverage_gaps(reduction.measurement_coverage, stamps[-1] / SECOND)
        # Then equality passes, but one nanosecond beyond ten seconds is incomplete.
        for stream in ("ipc_consumption", "ipc_matches", "decode"):
            assert ("coverage:" + stream in gaps) == bool(extra), (stream, gaps)


def test_pending_send_can_join_an_open_receive_after_its_own_interval_seals() -> None:
    # Given a pending send whose timestamp is at a sealed boundary.
    video = VideoMeasurements()
    video.start_window(0, 0)
    video.ipc("send", receive(1, 5 * SECOND))
    for stamp in (5, 10, 15):
        video.advance(stamp * SECOND)
    # When the receive arrives reordered but belongs to a still-open interval.
    video.ipc("receive", receive(1, 8 * SECOND))
    evidence = video.finish(15 * SECOND)
    # Then sealing uses receive time, not the earlier send time or arrival time.
    assert evidence.blocks[1].ipc_matches.count == 1
    assert evidence.blocks[1].ipc_matches.begin_ns == 5 * SECOND
    assert evidence.ipc_latency_ns == (3 * SECOND,)


def test_collector_freezes_before_final_poll_and_preserves_missing_raw_reads() -> None:
    # Given virtual-clock processes with startup endpoints and missing active resource reads.
    clock = [100 * SECOND]
    logs = []
    class LoggingProcess(Process):
        def __init__(self, command, *, stdout, **kwargs) -> None:
            super().__init__()
            logs.append(stdout)

        def terminate(self) -> None:
            emit({"event": "video_decode_completed", "session_ns": 0, "frame": 900,
                  "sequence": 900, "timestamp_ns": 900, "submitted_ns": clock[0] - 1,
                  "completed_ns": clock[0]})
            emit({"event": "media_timing", "audio_latency_ms": 199})
            emit({"event": "err", "code": "shutdown-diagnostic"})
            super().terminate()

    def emit(event: dict) -> None:
        logs[0].write(json.dumps(event) + "\n")
        logs[0].flush()

    def sleep(seconds: float) -> None:
        clock[0] += int(seconds * SECOND)
        stamp = clock[0]
        frame = stamp // SECOND
        for event in ({"event": "active", "startup_ms": 1},
                      {"event": "video_sent", "units": frame},
                      {"event": "audio_chunks", "count": frame},
                      {"event": "audio_write_completed", "scheduled_ns": stamp,
                       "scheduled_end_ns": stamp + 2, "completed_ns": stamp},
                      {"event": "video_presented", "scheduled_ns": stamp, "completed_ns": stamp},
                      {"event": "video_decode_completed", "session_ns": 0, "frame": frame,
                       "sequence": frame, "timestamp_ns": frame,
                       "submitted_ns": stamp - 2, "completed_ns": stamp - 1},
                      {"event": "decode_stats", "frames": 99999, "fps": 99999}):
            emit(event)

    with tempfile.TemporaryDirectory(prefix="perf-v2-capture-") as tmp:
        args = argparse.Namespace(receiver="fixture", display=None, no_display=True, port="fixture",
                                  duration_seconds=3, interval=1, startup_timeout_seconds=5,
                                  store=None, state_dir=tmp)
        zones = (ThermalZone("thermal_zone0:CPU", -2.0),)
        with (patch.object(collector.time, "monotonic_ns", side_effect=lambda: clock[0]),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector.subprocess, "Popen", LoggingProcess),
              patch.object(collector, "_cpu_ticks", side_effect=(100, None, 300, 400)),
              patch.object(collector, "_rss_mb", side_effect=(300.0, 100.0, 100.0, 100.0)),
              patch.object(collector, "_thermal_zones", side_effect=(zones, (replace(zones[0], temp_c=None),), zones, zones)),
              patch.object(collector, "_target_hardware_state", return_value={} )):
            # When the real collector samples then terminates and drains its logs.
            capture = collector.collect(args)
    # Then measurements freeze, while shutdown terminal diagnostics remain live.
    evidence = parse_capture(json.loads(json.dumps(capture)))
    reduction = reduce_capture(evidence, Window(SECOND, 4 * SECOND))
    assert reduction.measurement_coverage.decode.count == 3
    assert reduction.metrics["decode_frames"] == 3
    assert reduction.metrics["receiver_cpu_sustained_pct_one_core"] is None
    assert reduction.metrics["receiver_rss_peak_mb"] == 300
    assert reduction.metrics["rss_growth_pct"] < 0
    assert reduction.measurement_coverage.cpu.missing == 2
    assert reduction.measurement_coverage.thermal.missing == 1
    assert evidence.samples[1].receiver_cpu_ticks is None
    assert capture["events"]["audio_latency_ms"] is None
    assert capture["terminal_errors"] == [{"event": "err", "code": "shutdown-diagnostic"}]
    assert not set(collector._DECODE_KEYS.values()) & capture["events"].keys()
    assert not set(collector._IPC_KEYS.values()) & capture["events"].keys()


def test_thermal_inventory_keeps_failed_reads_with_stable_directory_identity() -> None:
    # Given two same-type zones, with one failed temperature read after enumeration.
    with tempfile.TemporaryDirectory(prefix="perf-v2-thermal-") as tmp:
        paths = [Path(tmp) / f"thermal_zone{number}" for number in range(2)]
        for path in paths:
            path.mkdir()
            (path / "type").write_text("CPU")
            (path / "temp").write_text("-2000")
        with patch.object(collector.glob, "glob", return_value=[str(path) for path in paths]):
            initial = collector._thermal_zones()
            (paths[1] / "temp").unlink()
            # When the expected active inventory is sampled again.
            observed = collector._thermal_zones(initial)
    # Then absence is explicit rather than filtered, and equal types remain distinct.
    assert initial == (ThermalZone("thermal_zone0:CPU", -2.0), ThermalZone("thermal_zone1:CPU", -2.0))
    assert observed == (initial[0], ThermalZone("thermal_zone1:CPU", None))


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} v2 capture tests")
