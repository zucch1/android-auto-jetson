#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/perf"))
from aa_projection_collect import EventParser
from aa_video_measurements import LIMIT, RETENTION_NS, IpcEndpoint


def ipc_event(kind: str, frame: int, observed: int) -> dict[str, int | str]:
    return {"event": kind, "producer_pid": 7, "consumer_pid": 8,
            "connection_ns": 100, "frame": frame, "sequence": frame * 3,
            "timestamp_ns": 987 + frame, "observed_ns": observed, "payload_bytes": 125_000}


def consume(parser: EventParser, events: list[dict[str, int | str]]) -> None:
    for event in events:
        parser._consume(json.dumps(event))


def test_ipc_uses_send_start_and_consumed_bytes_when_logs_are_reordered() -> None:
    # Given true send/receive endpoints 25 ms apart and an unrelated AV key.
    events = [ipc_event("video_ipc_send", 1, 1_000_000_100),
              ipc_event("video_ipc_receive", 1, 1_025_000_100),
              ipc_event("video_ipc_send", 2, 1_975_000_100),
              ipc_event("video_ipc_receive", 2, 2_000_000_100)]
    # When either file is polled first, including duplicate endpoint lines.
    for order in (events, list(reversed(events)), events[::2] + events[1::2]):
        parser = EventParser()
        consume(parser, order + order)
        # Then latency is not receive minus AV time and bytes are counted once.
        assert parser.metrics["ipc_p95_added_latency_ms"] == 25
        assert parser.metrics["ipc_throughput_mbps"] == 1
        assert len(parser.video_measurements.ipc_samples) == 2


def test_ipc_missing_stale_and_reset_samples_stay_absent() -> None:
    # Given endpoints that cannot represent one transfer.
    send = ipc_event("video_ipc_send", 1, 200)
    missing = ipc_event("video_ipc_receive", 2, 300)
    reset = ipc_event("video_ipc_receive", 1, 400)
    reset["connection_ns"] = 250
    stale = ipc_event("video_ipc_receive", 1, RETENTION_NS + 200)
    regressed = ipc_event("video_ipc_receive", 1, 150)
    wrong_size = ipc_event("video_ipc_receive", 1, 300)
    wrong_size["payload_bytes"] = 1
    wrong_process = ipc_event("video_ipc_receive", 1, 300)
    wrong_process["producer_pid"] = 9
    # When each unmatched path reaches the real collector.
    for receive in (missing, reset, stale, regressed, wrong_size, wrong_process):
        parser = EventParser()
        consume(parser, [send, receive])
        # Then no duration is fabricated for it.
        assert parser.metrics["ipc_p95_added_latency_ms"] is None, receive


def test_throughput_keeps_consumed_bytes_when_producer_log_is_far_ahead() -> None:
    # Given an unmatched future producer event and an older complete consumed AU.
    parser = EventParser()
    future = ipc_event("video_ipc_send", 2, 2 * RETENTION_NS)
    consumed = ipc_event("video_ipc_receive", 1, 1_000_000_100)
    # When latency correlation has already expired but consumption was observed.
    consume(parser, [future, consumed])
    # Then bytes still supply throughput without inventing a latency match.
    assert parser.metrics["ipc_throughput_mbps"] == 1
    assert parser.metrics["ipc_p95_added_latency_ms"] is None


def test_ipc_reconnect_rebinds_only_new_frame_identity() -> None:
    # Given an old unmatched send and a reconnect with restarted wire sequence.
    old = ipc_event("video_ipc_send", 1, 200)
    send = ipc_event("video_ipc_send", 2, 1_000_000_000)
    send["connection_ns"] = 500
    send["sequence"] = 0
    receive = ipc_event("video_ipc_receive", 2, 1_020_000_000)
    receive["connection_ns"] = 400
    receive["sequence"] = 0
    parser = EventParser()
    # When the new consumer log arrives before the producer log.
    consume(parser, [receive, old, send])
    # Then only the new matched transfer contributes.
    assert parser.metrics["ipc_p95_added_latency_ms"] == 20
    assert len(parser.video_measurements.ipc_samples) == 1
    assert len(parser.video_measurements.sends) == 1


def test_quantiles_count_and_fps_use_decode_completion_deltas() -> None:
    # Given 20 matched outputs with durations 1..20 ms, not AV timestamp deltas.
    events: list[dict[str, int | str]] = []
    for frame in range(1, 21):
        events.append({"event": "video_decode_completed", "session_ns": 100,
                       "frame": frame, "sequence": frame * 3, "timestamp_ns": 900 + frame,
                       "submitted_ns": 1_000_000_000, "completed_ns": 1_000_000_000 + frame * 1_000_000})
    parser = EventParser()
    # When reordered completions, duplicates and measured drops are consumed.
    consume(parser, list(reversed(events)) + events + [{"event": "decode_stats", "frame_drops": 3}])
    # Then nearest-rank quantiles and the measured output rate are exact.
    assert parser.metrics["decode_p50_ms"] == 10
    assert parser.metrics["decode_p95_ms"] == 19
    assert parser.metrics["decode_frames"] == 20
    assert parser.metrics["decode_fps"] == 1000
    assert parser.metrics["frame_drops"] == 3


def test_missing_decode_and_invalid_endpoints_never_supply_zero() -> None:
    # Given an empty capture or a duration beyond the decode retention policy.
    parser = EventParser()
    assert all(parser.metrics[key] is None for key in (
        "decode_frames", "decode_fps", "decode_p50_ms", "decode_p95_ms", "frame_drops"))
    invalid = {"event": "video_decode_completed", "session_ns": 100, "frame": 1,
               "sequence": 0, "timestamp_ns": 2, "submitted_ns": 200,
               "completed_ns": 5_000_000_200}
    # When invalid endpoint metadata reaches the parser boundary.
    consume(parser, [invalid])
    # Then invalid measurements remain absent and named, rather than zero.
    assert parser.metrics["decode_frames"] is None
    assert "decode_p95_ms" in parser.invalid_telemetry


def test_unmatched_storage_is_bounded_and_expired_keys_cannot_revive() -> None:
    # Given more pending sends than the fixed correlation capacity.
    parser = EventParser()
    # When the consumer never reports those frames.
    consume(parser, [ipc_event("video_ipc_send", frame, 1_000 + frame)
                     for frame in range(LIMIT + 50)])
    # Then eviction is accounted and cannot create a latency sample.
    assert len(parser.video_measurements.sends) == LIMIT
    assert parser.video_measurements.unmatched == 50
    assert parser.metrics["ipc_p95_added_latency_ms"] is None
    assert IpcEndpoint.parse({"observed_ns": True}) is None
    consume(parser, [ipc_event("video_ipc_receive", 0, 9_000),
                     ipc_event("video_ipc_send", 0, 1_000)])
    assert parser.metrics["ipc_p95_added_latency_ms"] is None


def test_decode_counts_continue_across_sessions_without_reusing_old_identity() -> None:
    # Given helper restart metadata with reused frame/sequence but a fresh session.
    first = {"event": "video_decode_completed", "session_ns": 100, "frame": 1,
             "sequence": 0, "timestamp_ns": 2, "submitted_ns": 1_000_000_000,
             "completed_ns": 1_010_000_000}
    second = dict(first, session_ns=2_000_000_000, submitted_ns=2_000_000_000,
                  completed_ns=2_020_000_000)
    parser = EventParser()
    # When the fresh completion and duplicate old completion arrive.
    consume(parser, [first, second, first])
    # Then both real completions count once and the old event cannot reset totals.
    assert parser.metrics["decode_frames"] == 2
    assert parser.metrics["decode_p95_ms"] == 20


def test_remote_stdin_bundle_contains_reducer_without_target_installation() -> None:
    # Given the production wrapper's actual stdin-bundle builder.
    tool = Path(__file__).resolve().parents[2] / "tools/perf"
    shell = (tool / "aa-projection-perf.sh").read_text()
    builder = shell.split('>"$BUFFER/collector.py" <<\'PY\'\n', 1)[1].split('\nPY\n', 1)[0]
    # When its output runs with an isolated cwd and no installed reducer module.
    bundled = subprocess.run([sys.executable, "-B", "-", str(tool)], input=builder,
                             text=True, capture_output=True, check=True, timeout=5)
    with tempfile.TemporaryDirectory(prefix="video-collector-bundle-") as cwd:
        result = subprocess.run([sys.executable, "-B", "-", "--help"], input=bundled.stdout,
                                text=True, capture_output=True, cwd=cwd, timeout=5)
    # Then the collector CLI loads, rather than failing its new import on target.
    assert result.returncode == 0, result.stderr
    assert "--duration-seconds" in result.stdout


def real_endpoints(probe: str) -> None:
    # Given a real fixture, receiver outlet, AF_UNIX channel and software decoder.
    fixture = Path(__file__).resolve().parents[1] / "fixtures/media/h264_720p30_baseline_90f.h264"
    # When the executable drives those endpoints without phones or target hardware.
    result = subprocess.run([probe, str(fixture)], text=True, capture_output=True, timeout=30)
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    parser = EventParser()
    for line in result.stdout.splitlines():
        parser._consume(line)
    events = [json.loads(line) for line in result.stdout.splitlines()]
    sent_identity = {event["frame"]: (event["sequence"], event["timestamp_ns"])
                     for event in events if event["event"] == "video_ipc_send"}
    received_identity = {event["frame"]: (event["sequence"], event["timestamp_ns"])
                         for event in events if event["event"] == "video_ipc_receive"}
    decoded_identity = {event["frame"]: (event["sequence"], event["timestamp_ns"])
                        for event in events if event["event"] == "video_decode_completed"}
    # Then all seven metrics come from actual endpoint events, not fixture constants.
    assert not parser.invalid_telemetry, parser.invalid_telemetry
    assert parser.metrics["frame_drops"] == 2, parser.metrics
    assert parser.metrics["decode_frames"] == 90, parser.metrics
    assert len(sent_identity) == 90
    assert sent_identity == received_identity == decoded_identity
    for key in ("decode_p50_ms", "decode_p95_ms", "decode_fps", "ipc_throughput_mbps",
                "ipc_p95_added_latency_ms"):
        assert parser.metrics[key] > 0, (key, parser.metrics)
    assert parser.video_measurements.receive_bytes == fixture.stat().st_size
    assert len(parser.video_measurements.ipc_samples) == 90


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    if len(sys.argv) == 2:
        real_endpoints(sys.argv[1])
    print(f"PASS {len(tests)} video measurement tests")
