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

- video held for the playout offset: the smoke cap (max <= 90 ms) and the soak
  rate (no pair > 150 ms, at most 1 > 90 ms per 30 min) hold. A Wi-Fi spike
  inside the buffer delays video arrivals (and its pairing) but not the audio
  schedule, which is the point of the buffer, so the audio model is unchanged
  by it (F9);
- a phone delivering audio in groups of B (window 11: max_arrival_burst 5;
  it captures in real time, so a group's chunks are late, its last on time)
  makes audio play (B - 1) chunk periods after the 120 ms buffer. The
  receiver reports that as the offset and holds video for it (N1): the gates
  hold for B = 2..5, while a fixed 120 ms hold fails them from B = 3;
- video not held: the contract sees the 120 ms audio lag and fails, so the
  offset is never hidden;
- the record without playout_offset_ns (wired) is judged exactly as before.
"""
from __future__ import annotations

import json
import random
import re
import sys
from pathlib import Path

CHUNK_NS = 2048 * 1_000_000_000 // 48000  # one A26 media chunk, 42.67 ms
FRAME_NS = 1_000_000_000 // 30
DEPTH_NS = 120_000_000
SINK_NS = 2_000_000    # sink handoff to playback slot
PAINT_NS = 9_000_000   # decode + paint after release
START_NS = 1_000_000_000
HOLD_CAP_NS = 400_000_000  # VideoOutlet::kMaxPresentationDelay (wireless)


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


def burst_audio_events(seconds: int, burst: int) -> list[dict]:
    """The router's records for a phone releasing groups of `burst` chunks.

    Chunk k is captured at k periods; its group is released when the group's
    last chunk is captured and arrives 10 ms later. The anchor is group 0's
    first (late) chunk, so the offset from on-time content is depth +
    (burst - 1) periods (AudioRouter test PlayoutOffsetIsTheDelayFromOnTime...).
    """
    offset = DEPTH_NS + (burst - 1) * CHUNK_NS
    events = []
    for k in range(seconds * 1_000_000_000 // CHUNK_NS):
        content = START_NS + 10_000_000 + k * CHUNK_NS
        slot = content + offset
        events.append({"event": "audio_write_completed", "scheduled_ns": content,
                       "scheduled_end_ns": content + CHUNK_NS, "playout_offset_ns": offset,
                       "completed_ns": slot - 60_000_000, "presented_ns": slot + SINK_NS,
                       "playback_slot_absence": None})
    return events, offset


def step_audio_events(seconds: int, high: int, low: int, drop_at: int, slewed: bool) -> list[dict]:
    """Writes whose reported offset drops from `high` to `low` at content `drop_at`.

    The old router reported the drop at once; the new one moves the offset by
    at most half the output time per write (AudioRouter, Neon P1-1).
    """
    events, offset = [], high
    for k in range(seconds * 1_000_000_000 // CHUNK_NS):
        content = START_NS + k * CHUNK_NS
        target = high if content < drop_at else low
        start_offset = offset
        if slewed:
            end_offset = start_offset + max(-CHUNK_NS // 2, min(CHUNK_NS // 2, target - start_offset))
        else:
            start_offset = end_offset = target
        offset = end_offset
        events.append({"event": "audio_write_completed", "scheduled_ns": content,
                       "scheduled_end_ns": content + CHUNK_NS, "playout_offset_ns": start_offset,
                       "completed_ns": content, "presented_ns": content + start_offset + SINK_NS,
                       "playback_slot_absence": None, "_end_offset": end_offset})
    return events


def wired_default_depth(root: Path) -> int:
    """kWiredPlayoutDepth, so the fixtures follow the production default."""
    header = (root / "include/aa/audio/AudioRouter.hpp").read_text(encoding="utf-8")
    found = re.search(r"kWiredPlayoutDepth\{([0-9']+)\}", header)
    assert found, "kWiredPlayoutDepth not found"
    return int(found[1].replace("'", ""))


def lossless_backlog_audio_events(seconds: int, depth: int, burst: int) -> list[dict]:
    """Records of a wired route that re-anchors on a `burst`-chunk backlog at 20 s.

    The offset target rises to depth + (burst - 1) chunks, then falls by the
    lossless rate correction (AudioRouter wanted_rate_ppm: 250 ppm per ms over a
    10 ms dead band up to 0.5%, plus 50 ppm per ms above 100 ms up to another
    0.5%); the reported offset follows by at most half the output time.
    """
    events, offset, target = [], depth, depth
    burst_at = START_NS + 20 * 1_000_000_000
    started = False
    for k in range(seconds * 1_000_000_000 // CHUNK_NS):
        content = START_NS + k * CHUNK_NS
        if not started and content >= burst_at:
            started, target = True, depth + (burst - 1) * CHUNK_NS
        elif started:
            excess = (target - depth) / 1e6
            ppm = min(5000.0, max(0.0, excess - 10) * 250) + min(5000.0, max(0.0, excess - 100) * 50)
            target -= int(CHUNK_NS * ppm / 1e6)
        start_offset = offset
        end_offset = start_offset + max(-CHUNK_NS // 2, min(CHUNK_NS // 2, target - start_offset))
        offset = end_offset
        events.append({"event": "audio_write_completed", "scheduled_ns": content,
                       "scheduled_end_ns": content + CHUNK_NS, "playout_offset_ns": start_offset,
                       "completed_ns": content, "presented_ns": content + start_offset + SINK_NS,
                       "playback_slot_absence": None, "_end_offset": end_offset})
    return events


def offset_at(audio: list[dict], t: int) -> int:
    for a in audio:
        if a["scheduled_ns"] <= t < a["scheduled_end_ns"]:
            span = a["scheduled_end_ns"] - a["scheduled_ns"]
            return a["playout_offset_ns"] + (a["_end_offset"] - a["playout_offset_ns"]) * (t - a["scheduled_ns"]) // span
    return audio[-1]["_end_offset"]


def wired_hold_cap(root: Path) -> int:
    """kLosslessVideoHoldCap: the wired receiver's hold cap."""
    header = (root / "tools/media_playout_options.hpp").read_text(encoding="utf-8")
    found = re.search(r"kLosslessVideoHoldCap\{([0-9]+)\}", header)
    assert found, "kLosslessVideoHoldCap not found"
    return int(found[1]) * 1_000_000


def outlet_video(seconds: int, audio: list[dict], new_outlet: bool, cap: int = HOLD_CAP_NS) -> list[dict]:
    """video_presented records through a model of VideoOutlet's hold queue.

    Old outlet: hold read at arrival, due = max(arrival + hold, previous due),
    so a frame after a drop is pinned behind the last held one. New outlet:
    hold read at release, never falling faster than half the arrival spacing
    below the previous frame's (VideoOutlet::front_hold_locked).
    """
    events, previous_due, previous = [], 0, None
    for n in range(seconds * 1_000_000_000 // FRAME_NS):
        arrival = START_NS + n * FRAME_NS + 10_000_000
        hold = min(cap, offset_at(audio, arrival))
        if new_outlet:
            if previous is not None:
                hold = max(hold, previous[1] - (arrival - previous[0]) // 2)
            due = arrival + hold
            previous = (arrival, hold)
        else:
            due = max(arrival + hold, previous_due)
        previous_due = due
        events.append({"event": "video_presented", "scheduled_ns": arrival, "completed_ns": due + PAINT_NS})
    return events


def gap_audio_events(seconds: int, high: int, gap_at: int, content_gap: int) -> list[dict]:
    """Writes at offset `high`, an underrun's content gap, then the router's resume.

    Across a gap the router may lower the offset by half the output gap og at
    once (its clamp's scheduled/previous_end are output-clock times); with
    content gap cg = og + (high - start) that is start = high - cg/3, toward
    the 120 ms depth it re-anchors with. It then slews by at most half a chunk
    per write.
    """
    events, offset, content = [], high, START_NS
    resumed = False
    while content < START_NS + seconds * 1_000_000_000:
        if not resumed and content >= gap_at:
            content += content_gap
            offset = max(DEPTH_NS, high - content_gap // 3)
            resumed = True
        target = high if not resumed else DEPTH_NS
        start_offset = offset
        end_offset = start_offset + max(-CHUNK_NS // 2, min(CHUNK_NS // 2, target - start_offset))
        offset = end_offset
        events.append({"event": "audio_write_completed", "scheduled_ns": content,
                       "scheduled_end_ns": content + CHUNK_NS, "playout_offset_ns": start_offset,
                       "completed_ns": content, "presented_ns": content + start_offset + SINK_NS,
                       "playback_slot_absence": None, "_end_offset": end_offset})
        content += CHUNK_NS
    return events


def gap_lookup(audio: list[dict], t: int, follow_router: bool) -> int:
    """AudioRenderWorker::playout_offset_at: inside a write, or across a gap."""
    for a, b in zip(audio, audio[1:]):
        if a["scheduled_ns"] <= t < a["scheduled_end_ns"]:
            return offset_at([a], t)
        if a["scheduled_end_ns"] <= t < b["scheduled_ns"]:
            if not follow_router:
                return a["_end_offset"]          # old: flat across the gap
            span = b["scheduled_ns"] - a["scheduled_end_ns"]
            return a["_end_offset"] + (b["playout_offset_ns"] - a["_end_offset"]) * (t - a["scheduled_end_ns"]) // span
    return offset_at(audio, t)


def outlet_video_lookup(seconds: int, audio: list[dict], follow_router: bool) -> list[dict]:
    """The new outlet (hold at release, half-rate down-slew) over either lookup."""
    events, previous = [], None
    for n in range(seconds * 1_000_000_000 // FRAME_NS):
        arrival = START_NS + n * FRAME_NS + 10_000_000
        hold = gap_lookup(audio, arrival, follow_router)
        if previous is not None:
            hold = max(hold, previous[1] - (arrival - previous[0]) // 2)
        previous = (arrival, hold)
        events.append({"event": "video_presented", "scheduled_ns": arrival,
                       "completed_ns": arrival + hold + PAINT_NS})
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

    # N1: grouped delivery, video held for the reported offset versus a fixed
    # 120 ms.
    for burst in (2, 3, 4, 5):
        audio, offset = burst_audio_events(70, burst)
        followed = video_events(66, False, offset, random.Random(burst))
        for clause in ("cap", "soak_rate"):
            result = judge(smoke, contract, audio, followed, clause)
            assert result["passed"], (burst, clause, result)
            assert abs(result["signed_p50_ms"]) < 20, (burst, result)
        fixed = video_events(66, False, DEPTH_NS, random.Random(burst))
        result = judge(smoke, contract, audio, fixed, "cap")
        if burst >= 3:
            assert not result["passed"], (burst, result)
        assert result["signed_p50_ms"] < -((burst - 1) * CHUNK_NS / 1e6) + 20, (burst, result)

    # Neon P1-1: a downward offset step (a group's peak leaving the window
    # 295 -> 167 ms, a re-anchor 400 -> 120 ms) through VideoOutlet's queue.
    for high, low in ((295_000_000, 167_000_000), (400_000_000, 120_000_000)):
        drop_at = START_NS + 30 * 1_000_000_000
        old_audio = step_audio_events(70, high, low, drop_at, slewed=False)
        old_result = judge(smoke, contract, old_audio, outlet_video(66, old_audio, new_outlet=False), "cap")
        assert not old_result["passed"] and old_result["signed_max_ms"] > 90, (high, low, old_result)
        new_audio = step_audio_events(70, high, low, drop_at, slewed=True)
        for clause in ("cap", "soak_rate"):
            new_result = judge(smoke, contract, new_audio, outlet_video(66, new_audio, new_outlet=True), clause)
            assert new_result["passed"], (high, low, clause, new_result)
            assert new_result["signed_max_ms"] < 20, (high, low, new_result)

    # Neon P1 #1 (round 4): an underrun's gap at a 295 ms offset (window 11's
    # p99) with video arriving all through it. Held for the old offset across
    # the gap, the frames after the resume sit above the audio's lower offset
    # and fail the cap; following the router's line across the gap they pass.
    for content_gap in (350_000_000, 500_000_000):
        audio = gap_audio_events(70, 295_000_000, START_NS + 30 * 1_000_000_000, content_gap)
        flat = judge(smoke, contract, audio, outlet_video_lookup(66, audio, follow_router=False), "cap")
        assert not flat["passed"] and flat["signed_max_ms"] > 90, (content_gap, flat)
        for clause in ("cap", "soak_rate"):
            followed = judge(smoke, contract, audio, outlet_video_lookup(66, audio, follow_router=True), clause)
            assert followed["passed"], (content_gap, clause, followed)
            assert followed["signed_max_ms"] < 30, (content_gap, followed)

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

    # Wired default playout (#228): the header's depth with video held to match
    # passes both cap and soak_rate; without the hold the contract sees the lag.
    wired_depth = wired_default_depth(root)
    wired_playout_audio = [
        {"event": "audio_write_completed",
         "scheduled_ns": START_NS + k * CHUNK_NS,
         "scheduled_end_ns": START_NS + (k + 1) * CHUNK_NS,
         "playout_offset_ns": wired_depth,
         "completed_ns": START_NS + k * CHUNK_NS + wired_depth - 20_000_000,
         "presented_ns": START_NS + k * CHUNK_NS + wired_depth + SINK_NS,
         "playback_slot_absence": None}
        for k in range(70 * 1_000_000_000 // CHUNK_NS)
    ]
    wired_matched_video = [
        {"event": "video_presented",
         "scheduled_ns": START_NS + n * FRAME_NS,
         "completed_ns": START_NS + n * FRAME_NS + wired_depth + PAINT_NS}
        for n in range(66 * 1_000_000_000 // FRAME_NS)
    ]
    for clause in ("cap", "soak_rate"):
        res = judge(smoke, contract, wired_playout_audio, wired_matched_video, clause)
        assert res["passed"], ("wired_playout", clause, res)
        assert abs(res["signed_p50_ms"]) < 15, res
        # #228 asks for at most 50 ms after warm-up (the cap allows 90).
        assert res["abs_max_ms"] <= 50.0, res

    # A burst after an underrun deepens the lossless buffer to 358 ms and it
    # drains by rate correction for about 45 s: video held for the reported
    # offset (VideoOutlet's slew and 400 ms cap) stays within 50 ms all along;
    # held for the configured depth only, it fails.
    deep_audio = lossless_backlog_audio_events(70, wired_depth, burst=8)
    peak = max(a["playout_offset_ns"] for a in deep_audio)
    assert wired_depth + 6 * CHUNK_NS < peak <= HOLD_CAP_NS, peak
    assert deep_audio[-1]["_end_offset"] < wired_depth + 15_000_000, deep_audio[-1]
    for clause in ("cap", "soak_rate"):
        res = judge(smoke, contract, deep_audio, outlet_video(66, deep_audio, new_outlet=True), clause)
        assert res["passed"], ("lossless_backlog", clause, res)
        assert res["abs_max_ms"] <= 50.0, res
    # A 16-chunk burst (re-review of 4a3f49f) is 700 ms deep: in sync under the
    # wired hold cap, up to 300 ms apart under wireless's.
    wired_cap = wired_hold_cap(root)
    burst_audio = lossless_backlog_audio_events(70, wired_depth, burst=16)
    assert HOLD_CAP_NS < max(a["playout_offset_ns"] for a in burst_audio) <= wired_cap
    for clause in ("cap", "soak_rate"):
        res = judge(smoke, contract, burst_audio,
                    outlet_video(66, burst_audio, new_outlet=True, cap=wired_cap), clause)
        assert res["passed"], ("sixteen_chunk_burst", clause, res)
        assert res["abs_max_ms"] <= 50.0, res
    clipped = judge(smoke, contract, burst_audio, outlet_video(66, burst_audio, new_outlet=True), "cap")
    assert not clipped["passed"] and clipped["abs_max_ms"] > 150.0, clipped

    stuck = judge(smoke, contract, deep_audio, wired_matched_video, "cap")
    assert not stuck["passed"], stuck
    wired_unheld_video = [
        {"event": "video_presented",
         "scheduled_ns": START_NS + n * FRAME_NS,
         "completed_ns": START_NS + n * FRAME_NS + PAINT_NS}
        for n in range(66 * 1_000_000_000 // FRAME_NS)
    ]
    unheld = judge(smoke, contract, wired_playout_audio, wired_unheld_video, "cap")
    assert not unheld["passed"] and unheld["signed_p50_ms"] < -40, unheld

    # Legacy arrival-model wired records (--audio-playout-ms 0, no playout_offset_ns) keep their meaning.
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
