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
``--port`` / ``--duration-ms`` / ``--store``.

Env knobs for the test:
  FAKE_RECEIVER_CRASH=1  exit non-zero before the duration elapses (crash path)
  FAKE_RECEIVER_GROW=1   grow RSS a little each tick (RSS-growth path)
"""
from __future__ import annotations

import json
import os
import sys
import time

DEFAULT_MS = 2000


def _duration_ms(argv: list[str]) -> int:
    for index, token in enumerate(argv):
        if token == "--duration-ms" and index + 1 < len(argv):
            return int(argv[index + 1])
    return DEFAULT_MS


def main() -> int:
    duration_ms = _duration_ms(sys.argv[1:])
    grow = os.environ.get("FAKE_RECEIVER_GROW") == "1"
    crash = os.environ.get("FAKE_RECEIVER_CRASH") == "1"
    ballast: list[bytes] = []

    print(json.dumps({"event": "ready", "state": "negotiating"}), flush=True)
    print(json.dumps({"event": "active", "startup_ms": 120}), flush=True)

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

    print(json.dumps({"event": "stopping", "duration_ms": duration_ms}), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
