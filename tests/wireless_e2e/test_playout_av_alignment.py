# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Wireless playout keeps the signed A/V contract honest (credit window + playout, 2026-11-20).

The receiver plays wireless audio on a playout clock 120 ms behind arrival and
holds video for the same 120 ms. audio_write_completed reports the content's
arrival-clock schedule (scheduled_ns) plus playout_offset_ns, so its
presented - scheduled latency includes the buffer, exactly as the display's
paint latency includes the video hold. This drives the real A/V pairing
(tools/wired-qa/aa_drive_smoke.av_pairs) and contract (tools/perf/aa_av_contract)
with that model:

- video held for the playout depth: the smoke cap (max <= 90 ms) and the soak
  rate (no pair > 150 ms, at most 1 > 90 ms per 30 min) hold, through a Wi-Fi
  spike inside the buffer;
- video not held: the contract sees the 120 ms audio lag and fails, so the
  offset is never hidden;
- the record without playout_offset_ns (wired) is judged exactly as before.
"""
from __future__ import annotations

import json
import random
import sys
from pathlib import Path

CHUNK_NS = 2048 * 1_000_000_000 // 48000  # one A26 media chunk, 42.67 ms
FRAME_NS = 1_000_000_000 // 30
DEPTH_NS = 120_000_000
SINK_NS = 2_000_000    # sink handoff to playback slot
PAINT_NS = 9_000_000   # decode + paint after release
START_NS = 1_000_000_000


def one_way_ns(t_ns: int, spike: bool) -> int:
    """Baseline 10 ms one-way; a 2 s spike to a 150 ms round trip (75 ms one-way)."""
    second = (t_ns - START_NS) // 1_000_000_000
    return 75_000_000 if spike and 30 <= second < 32 else 10_000_000


def audio_events(seconds: int, spike: bool) -> list[dict]:
    """audio_write_completed records of the playout model, one per chunk.

    Chunks play back to back from the anchor (first arrival + depth); the
    record's scheduled_ns is that slot minus the playout offset.
    """
    anchor = START_NS + one_way_ns(START_NS, spike) + DEPTH_NS
    events = []
    for k in range(seconds * 1_000_000_000 // CHUNK_NS):
        slot = anchor + k * CHUNK_NS
        events.append({"event": "audio_write_completed",
                       "scheduled_ns": slot - DEPTH_NS,
                       "scheduled_end_ns": slot + CHUNK_NS - DEPTH_NS,
                       "playout_offset_ns": DEPTH_NS,
                       "completed_ns": slot - 60_000_000,
                       "presented_ns": slot + SINK_NS,
                       "playback_slot_absence": None})
    return events


def video_events(seconds: int, spike: bool, hold_ns: int, rng: random.Random) -> list[dict]:
    """video_presented records: stamped at arrival, painted hold_ns + paint later."""
    events = []
    for n in range(seconds * 1_000_000_000 // FRAME_NS):
        captured = START_NS + n * FRAME_NS
        arrival = captured + one_way_ns(captured, spike) + rng.randrange(0, 4_000_000)
        events.append({"event": "video_presented", "scheduled_ns": arrival,
                       "completed_ns": arrival + hold_ns + PAINT_NS + rng.randrange(0, 6_000_000)})
    return events


def judge(smoke, contract, audio: list[dict], video: list[dict], clause: str) -> dict:
    valid, invalid = smoke._valid_audio(audio)
    assert invalid == 0
    pairs = smoke.av_pairs(video, valid)
    start = video[0]["completed_ns"]
    end = start + 60 * 1_000_000_000
    return contract.evaluate(pairs, [(s["scheduled_ns"], s["scheduled_end_ns"]) for s in valid],
                             start, end, max_clause=clause)


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(root / "tools" / "perf"))
    sys.path.insert(0, str(root / "tools" / "wired-qa"))
    import aa_av_contract as contract  # noqa: PLC0415
    import aa_drive_smoke as smoke  # noqa: PLC0415

    for spike in (False, True):
        audio = audio_events(70, spike)
        matched = video_events(66, spike, DEPTH_NS, random.Random(7))
        for clause in ("cap", "soak_rate"):
            result = judge(smoke, contract, audio, matched, clause)
            assert result["passed"], (spike, clause, result)
            assert result["coverage"] >= contract.AV_COVERAGE_MIN, result
            assert abs(result["signed_p50_ms"]) < 20, result
        # The video hold and the playout offset cancel: the median skew is the
        # paint-versus-sink difference, not the buffer.
        unheld = video_events(66, spike, 0, random.Random(7))
        result = judge(smoke, contract, audio, unheld, "cap")
        assert not result["passed"], result
        assert result["signed_p50_ms"] < -100, result

    # Neon P2-4: the collector drops any write whose scheduled_ns does not
    # advance. When the playout offset steps up (a re-anchor, a burst showing
    # a later content clock), reporting scheduled - offset naively runs
    # backwards and loses writes; the receiver instead lets the content
    # interval advance at half rate until it has caught up (AudioRouter), so
    # every write is kept.
    import aa_projection_collect as collector  # noqa: PLC0415
    piece = 50_000_000
    targets = [DEPTH_NS] * 40 + [DEPTH_NS + 80_000_000] * 40 + [DEPTH_NS] * 40
    naive, reported = [], []
    last_end = None
    for n, target in enumerate(targets):
        slot = START_NS + n * piece
        naive.append((slot - target, slot + piece - target, target))
        start, end = slot - target, slot + piece - target
        if last_end is not None and start < last_end:
            start = last_end
            end = max(end, start + piece // 2)
        reported.append((start, end, slot - start))
        last_end = end
    for name, records, expected_all in (("naive", naive, False), ("reported", reported, True)):
        parser = collector.EventParser()
        for start, end, offset in records:
            parser._consume(json.dumps({
                "event": "audio_write_completed", "scheduled_ns": start, "scheduled_end_ns": end,
                "playout_offset_ns": offset, "completed_ns": start + offset - 40_000_000,
                "presented_ns": start + offset + SINK_NS, "playback_slot_absence": None}))
        assert (parser.audio_writes == len(records)) == expected_all, (name, parser.audio_writes)

    # Wired records (no playout_offset_ns) keep their meaning.
    wired = [{"event": "audio_write_completed", "scheduled_ns": START_NS + k * CHUNK_NS,
              "scheduled_end_ns": START_NS + (k + 1) * CHUNK_NS,
              "completed_ns": START_NS + k * CHUNK_NS,
              "presented_ns": START_NS + k * CHUNK_NS + SINK_NS, "playback_slot_absence": None}
             for k in range(70 * 1_000_000_000 // CHUNK_NS)]
    wired_video = [{"event": "video_presented", "scheduled_ns": START_NS + n * FRAME_NS,
                    "completed_ns": START_NS + n * FRAME_NS + PAINT_NS}
                   for n in range(66 * 1_000_000_000 // FRAME_NS)]
    assert judge(smoke, contract, wired, wired_video, "cap")["passed"]
    print("playout A/V alignment: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
