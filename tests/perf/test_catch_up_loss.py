#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/perf/test_catch_up_loss.py
"""Catch-up loss (#154) reaches the qualification capture and report.

The wired receiver drops a post-stall placement backlog past its 90 ms
watermark in one run and logs ``audio_catch_up``. That loss is not a sink
stall, so it must appear as its own informational metrics (events, frames,
ms), never as an ``audio_stall_*`` dropout and never as a gate.
"""
from __future__ import annotations

import copy
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import aa_perf_report as reporter  # noqa: E402
import aa_perf_schema as schema  # noqa: E402
import aa_projection_collect as collector  # noqa: E402
from test_v3_av_contract import result, v3  # noqa: E402
from test_v2_coverage import synthetic  # noqa: E402

KEYS = ("audio_catch_up_events", "audio_catch_up_frames", "audio_catch_up_ms")
STALL_KEYS = ("audio_stall_dropouts", "audio_stall_dropped_chunks", "audio_stall_unrecovered")


def burst_lines() -> list[str]:
    """The worst-case burst as the receiver logs it: 12 fully dropped 2048-frame
    parents (1808 catch-up frames each beyond the 240-frame drift trim), with a
    sink reopen after the eighth that resets the router's frames_total."""
    lines, total = [], 0
    for index in range(12):
        if index == 8:
            total = 0  # a new router after the reopen: frames_total starts again
        total += 1808
        lines.append('{"event":"audio_catch_up","frames":1808,"frames_total":%d,"rate":48000}' % total)
    return lines


BURST_FRAMES = 12 * 1808
BURST_MS = BURST_FRAMES / 48.0  # 452 ms at 48 kHz


def parse(lines: list[str]) -> collector.EventParser:
    parser = collector.EventParser()
    for line in lines:
        parser._consume(line)
    return parser


def test_parser_counts_catch_up_separately_from_stall_bypass() -> None:
    parser = parse(burst_lines())
    assert parser.metrics["audio_catch_up_events"] == 12, parser.metrics
    # Summed increments, not the last frames_total (3616 after the reset).
    assert parser.metrics["audio_catch_up_frames"] == BURST_FRAMES, parser.metrics
    assert abs(parser.metrics["audio_catch_up_ms"] - BURST_MS) < 1e-3, parser.metrics
    for key in STALL_KEYS:
        assert parser.metrics[key] == 0, (key, parser.metrics)
    assert parser.terminal_errors == [] and parser.invalid_telemetry == []


def test_parser_reports_zero_without_catch_up_and_null_when_malformed() -> None:
    assert [parse([]).metrics[key] for key in KEYS] == [0, 0, 0.0]
    for bad in ('{"event":"audio_catch_up","frames":1808,"frames_total":1808}',
                '{"event":"audio_catch_up","frames":-1,"frames_total":1,"rate":48000}',
                '{"event":"audio_catch_up","frames":1.5,"frames_total":1,"rate":48000}',
                '{"event":"audio_catch_up","frames":true,"frames_total":1,"rate":48000}',
                '{"event":"audio_catch_up","frames":1808,"frames_total":1808,"rate":0}'):
        parser = parse([*burst_lines()[:3], bad, *burst_lines()[3:]])
        # A partial sum would understate the loss: unknown, never undercounted.
        assert [parser.metrics[key] for key in KEYS] == [None, None, None], (bad, parser.metrics)
        # Informational only: a malformed loss record does not block the run.
        assert parser.invalid_telemetry == [], bad


def with_catch_up(raw: dict, events: int, frames: int, ms: float) -> dict:
    raw = copy.deepcopy(raw)
    raw["events"].update({"audio_catch_up_events": events, "audio_catch_up_frames": frames,
                          "audio_catch_up_ms": ms, "audio_stall_dropouts": 0,
                          "audio_stall_dropped_chunks": 0, "audio_stall_unrecovered": 0})
    return raw


def test_report_carries_catch_up_loss_without_gating_it() -> None:
    clean = result(v3())
    lossy = result(with_catch_up(v3(), 12, BURST_FRAMES, BURST_MS))
    # Informational: the verdict, violations and blockers are unchanged.
    assert lossy["verdict"] == clean["verdict"] == "PASS", (lossy["violations"], lossy["blocked_on"])
    assert lossy["violations"] == clean["violations"]
    assert lossy["blocked_on"] == clean["blocked_on"]
    metrics = lossy["metrics"]
    expected = {"audio_catch_up_events": (12, "events"),
                "audio_catch_up_frames": (BURST_FRAMES, "frames"),
                "audio_catch_up_ms": (BURST_MS, "ms")}
    for key, (value, unit) in expected.items():
        entry = metrics[key]
        assert entry["value"] == value and entry["unit"] == unit, (key, entry)
        assert entry["gated"] is False and entry["status"] == "INFORMATIONAL", (key, entry)
    # Not relabelled as a stall bypass.
    for key in STALL_KEYS:
        assert metrics[key]["value"] == 0, (key, metrics[key])
    assert not schema.validate(lossy, schema.load_schema(schema.SCHEMA_V3))


def test_schema_rejects_a_gated_or_mislabelled_catch_up_metric() -> None:
    doc = result(with_catch_up(v3(), 1, 1808, 1808 / 48))
    loaded = schema.load_schema(schema.SCHEMA_V3)
    for key, field, value in (("audio_catch_up_ms", "gated", True),
                              ("audio_catch_up_ms", "status", "PASS"),
                              ("audio_catch_up_frames", "unit", "chunks"),
                              ("audio_catch_up_events", "unit", "frames"),
                              ("audio_catch_up_frames", "value", -1)):
        bad = copy.deepcopy(doc)
        bad["metrics"][key][field] = value
        assert schema.validate(bad, loaded), (key, field, value)


def test_pre_catch_up_v3_capture_reports_null_and_v2_is_unchanged() -> None:
    doc = result(v3())  # a v3 capture recorded before audio_catch_up existed
    for key in KEYS:
        assert doc["metrics"][key]["value"] is None, (key, doc["metrics"][key])
        assert doc["metrics"][key]["status"] == "INFORMATIONAL"
    assert doc["verdict"] == "PASS"
    historical = result(synthetic(1800))
    assert historical["schema"] == "aa-projection-perf/2"
    assert not any(key in historical["metrics"] for key in KEYS)


def test_report_rejects_inconsistent_catch_up_evidence() -> None:
    partial = v3()
    partial["events"]["audio_catch_up_frames"] = 1808
    for raw in (partial, with_catch_up(v3(), 1, -1808, 1.0),
                with_catch_up(v3(), 0, 1808, 37.666)):
        try:
            result(raw)
        except reporter.ContractError:
            continue
        raise AssertionError(raw["events"])


def test_collector_capture_replays_catch_up_into_the_report() -> None:
    # Given the receiver log of a stall burst survived by catch-up, no bypass.
    with tempfile.TemporaryDirectory(prefix="perf-catch-up-") as base:
        env = dict(os.environ, FAKE_RECEIVER_RAW_TELEMETRY="\n".join(burst_lines()))
        proc = subprocess.run(
            [sys.executable, "-B", str(ROOT / "tools/perf/aa_projection_collect.py"),
             "--receiver", str(ROOT / "tests/perf/fixtures/wired_perf/fake_receiver.py"),
             "--port", "fixture", "--no-display", "--duration-seconds", "1",
             "--startup-timeout-seconds", "1", "--interval", "0.1",
             "--state-dir", str(Path(base) / "state"),
             # Synthetic receiver is not a CMake build: skip the build guard.
             "--allow-unoptimized-build"],
            env=env, capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0, proc.stderr
        capture = json.loads(proc.stdout)
    # When the collector writes its capture.
    assert capture["schema"] == "aa-projection-perf-capture/3"
    events = capture["events"]
    # Then the capture carries the loss in frames and ms, apart from the stall counters.
    assert events["audio_catch_up_events"] == 12, events
    assert events["audio_catch_up_frames"] == BURST_FRAMES, events
    assert abs(events["audio_catch_up_ms"] - BURST_MS) < 1e-3, events
    for key in STALL_KEYS:
        assert events[key] == 0, (key, events)
    # And the report built from that capture shows it, informational only.
    doc = reporter.build_result(copy.deepcopy(capture), duration_requested=None,
                                now="2026-11-18T00:00:00Z", capture_path=None, capture_sha256=None)
    assert doc["metrics"]["audio_catch_up_frames"]["value"] == BURST_FRAMES
    assert abs(doc["metrics"]["audio_catch_up_ms"]["value"] - BURST_MS) < 1e-3
    assert doc["metrics"]["audio_stall_dropped_chunks"]["value"] == 0
    assert "audio_catch_up_ms" not in {v["metric"] for v in doc["violations"]}
    assert not any(name.startswith("audio_catch_up") for name in doc["blocked_on"]), doc["blocked_on"]
    assert not schema.validate(doc, schema.load_schema(schema.SCHEMA_V3))


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} catch-up loss accounting tests")
