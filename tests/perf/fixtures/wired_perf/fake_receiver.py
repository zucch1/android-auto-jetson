#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Deterministic fake wired receiver for the task-32 collector test.

Speaks the same newline-delimited JSON event stream as
``tools/wired-receiver/main.cpp`` (ready/active/video_sent/audio_chunks and a
``diag`` ``decode_stats`` event) so ``aa_projection_collect.py`` can be
exercised on the host without hardware. Accepts (and ignores) the receiver CLI
``--port`` / ``--duration-ms`` / ``--store`` / ``--video-socket`` /
``--input-socket`` / ``--require-video-consumer`` (the managed-display consumer
gate: accepted here, never emulated - this fixture fakes no hardware
qualification).

Env knobs for the test:
  FAKE_RECEIVER_CRASH=1  exit non-zero before the duration elapses (crash path)
  FAKE_RECEIVER_GROW=1   grow RSS a little each tick (RSS-growth path)
  FAKE_RECEIVER_MEDIA_TIMING=1  emit a ``media_timing`` event carrying the two
                         task-48 gated metrics (av_sync_ms / audio_latency_ms);
                         the values come from FAKE_RECEIVER_AV_SYNC_MS /
                         FAKE_RECEIVER_AUDIO_LATENCY_MS (defaults 12.5 / 40.5)
                         so a test can vary the injected measurement and prove
                         the collector reports the measured value, not a
                         constant. A comma-separated list emits successive
                         values across ticks (last value repeats). A
                         non-numeric or non-finite token is passed through as a
                         JSON string: the collector must keep the metric null
                         (BLOCKED), never fabricate a 0. Without the knob the
                         fields are absent and the collector keeps them null
                         -> BLOCKED (fail closed).
  FAKE_RECEIVER_HELD_MEDIA=<json>  a media record stamped (emit_ns) 1.5 s into
                         the run but written only during the shutdown drain,
                         like a record the real receiver's stalled stdout
                         delivered late (#90 review 4). FAKE_RECEIVER_HELD_UNSTAMPED=1
                         writes it without emit_ns.
  Every media record carries the receiver's emit_ns (CLOCK_MONOTONIC).
  --require-usb-serial S emulates the receiver's same-phone pin (#137):
  FAKE_RECEIVER_ATTACHED_SERIAL (default S) is the phone on the port. A
  different one fails the first link (err code phone_serial_mismatch, exit 3,
  before ready); FAKE_RECEIVER_SWAP_AFTER_MS swaps in "OTHERPHONE9" after
  that long, so the reconnect fails the same way (exit 1).
"""
from __future__ import annotations

import json
import math
import os
import signal
import sys
import time

DEFAULT_MS = 2000


def _measurement(token: str) -> float | str:
    try:
        value = float(token)
    except ValueError:
        return token
    return value if math.isfinite(value) else token


def _series(raw: str | None, default: str) -> list[float | str]:
    return [_measurement(token.strip()) for token in (raw or default).split(",")]


def _duration_ms(argv: list[str]) -> int:
    for index, token in enumerate(argv):
        if token == "--duration-ms" and index + 1 < len(argv):
            return int(argv[index + 1])
    return DEFAULT_MS


def _option(argv: list[str], name: str) -> str | None:
    for index, token in enumerate(argv):
        if token == name and index + 1 < len(argv):
            return argv[index + 1]
    return None


MISMATCH = '{"event":"err","detail":"link","code":"phone_serial_mismatch"}'


HELD: dict = {}


def _media(record: dict) -> str:
    """A media record as the receiver writes it: emit_ns stamped at the source."""
    return json.dumps(record | {"emit_ns": time.monotonic_ns()})


def _finish() -> None:
    """The real receiver's shutdown tail: `stopping`, late summaries, log_complete."""
    if HELD.get("record") is not None:
        print(json.dumps(HELD.pop("record")), flush=True)
    late = os.environ.get("FAKE_RECEIVER_LATE_SUMMARY")
    if late:
        print(late, flush=True)
    if os.environ.get("FAKE_RECEIVER_NO_LOG_COMPLETE") != "1":
        print(json.dumps({"event": "log_complete", "runtime_dropped": 0, "state_overflow": 0,
                          "media_rate_limited": 0, "media_backpressure": 0, "shutdown_dropped": 0,
                          "max_delivery_lag_ms": 0} | json.loads(
                              os.environ.get("FAKE_RECEIVER_LOG_COMPLETE", "{}"))), flush=True)


def _terminated(*_: object) -> None:
    print(json.dumps({"event": "stopping", "duration_ms": 0}), flush=True)
    _finish()
    sys.exit(0)


def main() -> int:
    signal.signal(signal.SIGTERM, _terminated)
    duration_ms = _duration_ms(sys.argv[1:])
    required = _option(sys.argv[1:], "--require-usb-serial")
    attached = os.environ.get("FAKE_RECEIVER_ATTACHED_SERIAL", required or "")
    swap_after = os.environ.get("FAKE_RECEIVER_SWAP_AFTER_MS")
    if required is not None and attached != required:
        print(MISMATCH, flush=True)
        return 3
    grow = os.environ.get("FAKE_RECEIVER_GROW") == "1"
    crash = os.environ.get("FAKE_RECEIVER_CRASH") == "1"
    media_timing = os.environ.get("FAKE_RECEIVER_MEDIA_TIMING") == "1"
    av_series = _series(os.environ.get("FAKE_RECEIVER_AV_SYNC_MS"), "12.5")
    audio_series = _series(os.environ.get("FAKE_RECEIVER_AUDIO_LATENCY_MS"), "40.5")
    ballast: list[bytes] = []

    print(json.dumps({"event": "ready", "state": "negotiating"}), flush=True)
    print(json.dumps({"event": "active", "startup_ms": 120}), flush=True)
    raw_telemetry = os.environ.get("FAKE_RECEIVER_RAW_TELEMETRY")
    if raw_telemetry is not None:
        print('{"event":"media_timing","av_sync_ms":12,"audio_latency_ms":40}', flush=True)
        print(raw_telemetry, flush=True)

    start = time.monotonic()
    tick = 0
    held = os.environ.get("FAKE_RECEIVER_HELD_MEDIA")
    while (time.monotonic() - start) * 1000 < duration_ms:
        time.sleep(0.2)
        tick += 1
        if held is not None and "record" not in HELD and time.monotonic() - start >= 1.5:
            record = json.loads(held)
            if os.environ.get("FAKE_RECEIVER_HELD_UNSTAMPED") != "1":
                record["emit_ns"] = time.monotonic_ns()
            HELD["record"] = record
        if crash and (time.monotonic() - start) > 0.6:
            return 2
        if (required is not None and swap_after is not None
                and (time.monotonic() - start) * 1000 >= int(swap_after)):
            print(MISMATCH, flush=True)  # the reconnect found another phone
            return 1
        if grow and tick % 2 == 0:
            ballast.append(bytearray(256 * 1024))
        if tick % 5 == 0:
            print(json.dumps({"event": "video_sent", "units": tick * 3}), flush=True)
            print(json.dumps({"event": "audio_chunks", "count": tick}), flush=True)
            print(
                json.dumps(
                    {
                        "event": "diag",
                        "kind": "decode_stats",
                        "session": 1,
                        "mono_ns": tick * 1000,
                        "frames": tick * 3,
                        "dropped": 0,
                        "p50_latency_ms": 20.0,
                        "p95_latency_ms": 25.0,
                        "fps": 30.0,
                    }
                ),
                flush=True,
            )
            if media_timing:
                index = tick // 5 - 1
                av = av_series[min(index, len(av_series) - 1)]
                stamp = tick * 1_000_000_000
                print(_media({"event": "audio_write_completed", "scheduled_ns": stamp,
                              "scheduled_end_ns": stamp + 20_000_000,
                              "completed_ns": stamp}), flush=True)
                print(_media({"event": "video_presented", "frame": tick,
                              "scheduled_ns": stamp,
                              "completed_ns": stamp + round(av * 1e6)
                              if isinstance(av, float) else av}), flush=True)
                print(
                    _media(
                        {
                            "event": "media_timing",
                            "audio_scheduling_skew_ms": 0,
                            "audio_latency_ms": audio_series[
                                min(index, len(audio_series) - 1)
                            ],
                        }
                    ),
                    flush=True,
                )

    print(json.dumps({"event": "stopping", "duration_ms": duration_ms}), flush=True)
    _finish()
    return 0


if __name__ == "__main__":
    sys.exit(main())
