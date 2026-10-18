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


def main() -> int:
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
    duration_ms = _duration_ms(sys.argv[1:])
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
    while (time.monotonic() - start) * 1000 < duration_ms:
        time.sleep(0.2)
        tick += 1
        if crash and (time.monotonic() - start) > 0.6:
            return 2
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
                print(json.dumps({"event": "audio_write_completed", "scheduled_ns": stamp,
                                  "scheduled_end_ns": stamp + 20_000_000,
                                  "completed_ns": stamp}), flush=True)
                print(json.dumps({"event": "video_presented", "frame": tick,
                                  "scheduled_ns": stamp,
                                  "completed_ns": stamp + round(av * 1e6)
                                  if isinstance(av, float) else av}), flush=True)
                print(
                    json.dumps(
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
    return 0


if __name__ == "__main__":
    sys.exit(main())
