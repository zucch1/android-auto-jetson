#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Signed A/V contract shared by the drive smoke and the soak stages (#137, #71).

Owner decision 2026-11-16, replacing "max |skew| <= 50 ms". The rationale for
every limit is in docs/qualification/wired-perf-runbook.md (Signed A/V
contract). The math lives here once: tools/wired-qa/aa_drive_smoke.py judges a
drive with it and tools/perf/aa_perf_report.py judges a soak stage with it.

skew = (video paint completed - video scheduled) - (audio presented - audio
scheduled), in ms: positive means the video endpoint is later than the audio
endpoint. These are internal software endpoints (Qt paint end, sink playback
slot), not light and sound; the physical offset and its sign are #58.
"""
from __future__ import annotations

import bisect
import math
from collections.abc import Iterable, Sequence

# ---- Limits. Do not edit to make a run pass. --------------------------------
# (a) Sustained offset: the mean skew of every full half-open 5 s window, anchored
#     on paint completion, is within +/-40 ms (EBU R37's tighter, sound-early,
#     end-to-end bound; symmetric until #58 measures the physical offset).
AV_MEAN_WINDOW_S = 5
AV_MEAN_MAX_ABS_MS = 40.0
# (b) Tail: the nearest-rank 99th percentile of |skew| is <= 50 ms (the previous
#     contract's number; 1.5 frame periods at 30 fps).
AV_P99_ABS_MAX_MS = 50.0
# (c) Cap: max |skew| <= 90 ms (ITU-R BT.1359-1's sound-early acceptability bound).
#     The drive smoke's clause (max_clause="cap").
AV_MAX_ABS_MS = 90.0
# (c') Soak stages (max_clause="soak_rate", owner decision 2026-11-17): a
#     maximum over a fixed limit grows with run length, so a long stage judges
#     the rate of excursions instead. An excursion is a pair with
#     |skew| > AV_SOAK_EXCURSION_MS; every pair counts, consecutive pairs of one
#     spike separately. A stage may have floor(active window /
#     AV_SOAK_EXCURSION_PERIOD_S) of them (1 in 1800 s, 4 in 7200 s), and no
#     pair may exceed the hard cap AV_SOAK_HARD_CAP_MS.
AV_SOAK_EXCURSION_MS = 90.0
AV_SOAK_EXCURSION_PERIOD_S = 1800
AV_SOAK_EXCURSIONS_PER_S = 1 / AV_SOAK_EXCURSION_PERIOD_S
AV_SOAK_HARD_CAP_MS = 150.0
# At most this many excursions are listed one by one (the count is always exact).
EXCURSIONS_LISTED_MAX = 1000
# Coverage: the union of valid audio slots' scheduled intervals, intersected
# with the judged window, must cover this share of it. No audio, or too
# little, is a FAIL, never "n/a".
AV_COVERAGE_MIN = 0.90

LIMITS = {"window_s": AV_MEAN_WINDOW_S, "window_mean_abs_ms": AV_MEAN_MAX_ABS_MS,
          "p99_abs_ms": AV_P99_ABS_MAX_MS, "max_abs_ms": AV_MAX_ABS_MS,
          "coverage_min": AV_COVERAGE_MIN}
SOAK_LIMITS = {"window_s": AV_MEAN_WINDOW_S, "window_mean_abs_ms": AV_MEAN_MAX_ABS_MS,
               "p99_abs_ms": AV_P99_ABS_MAX_MS, "excursion_ms": AV_SOAK_EXCURSION_MS,
               "excursions_per_s": AV_SOAK_EXCURSIONS_PER_S,
               "excursion_period_s": AV_SOAK_EXCURSION_PERIOD_S,
               "hard_cap_ms": AV_SOAK_HARD_CAP_MS, "coverage_min": AV_COVERAGE_MIN}
MAX_CLAUSES = {"cap": LIMITS, "soak_rate": SOAK_LIMITS}


def excursion_allowance(start_ns: int | None, end_ns: int | None) -> int:
    """floor(window seconds / AV_SOAK_EXCURSION_PERIOD_S), exact on integer ns."""
    if start_ns is None or end_ns is None or end_ns <= start_ns:
        return 0
    return (end_ns - start_ns) // (AV_SOAK_EXCURSION_PERIOD_S * 10**9)


def merge_intervals(intervals: Iterable[tuple[int, int]]) -> list[tuple[int, int]]:
    """Union of half-open [start, end) intervals, sorted and non-overlapping."""
    merged: list[list[int]] = []
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [(start, end) for start, end in merged]


def coverage(intervals: Iterable[tuple[int, int]], start_ns: int, end_ns: int) -> float:
    """Share of [start_ns, end_ns] covered by the union of the intervals."""
    if end_ns <= start_ns:
        return 0.0
    covered = 0
    for lo, hi in merge_intervals(intervals):
        lo, hi = max(lo, start_ns), min(hi, end_ns)
        if hi > lo:
            covered += hi - lo
    return covered / (end_ns - start_ns)


def window_means(pairs: Sequence[tuple[int, float]], start_ns: int, end_ns: int,
                 width_s: float = AV_MEAN_WINDOW_S) -> tuple[float, float] | None:
    """(min, max) sample mean over every full half-open window [t, t+width) in [start, end].

    ``pairs`` are (paint completion ns, signed skew ms), sorted by time. A pair
    exactly at ``t`` is inside a window, one exactly at ``t + width`` is not;
    only full windows count (``start_ns <= t`` and ``t + width <= end_ns``).
    Membership is constant between change points, so evaluating ``start_ns``
    and every change point visits every distinct window: a pair at ``p``
    enters at ``t = p - width + 1`` and leaves at ``t = p + 1`` (integer ns).
    Windows without pairs have no mean.
    """
    width = int(width_s * 10**9)
    if not pairs or end_ns - start_ns < width:
        return None
    times = [t for t, _ in pairs]
    prefix = [0.0]
    for _, skew in pairs:
        prefix.append(prefix[-1] + skew)
    latest = end_ns - width
    candidates = {start_ns}
    for p in times:
        for t in (p - width + 1, p + 1):  # p enters / p leaves
            if start_ns <= t <= latest:
                candidates.add(t)
    low = high = None
    for t in candidates:
        i, j = bisect.bisect_left(times, t), bisect.bisect_left(times, t + width)
        if j > i:
            mean = (prefix[j] - prefix[i]) / (j - i)
            low = mean if low is None else min(low, mean)
            high = mean if high is None else max(high, mean)
    return None if low is None else (low, high)


def rank(sorted_values: Sequence[float], fraction: float) -> float:
    """Nearest-rank percentile of already sorted values."""
    return sorted_values[max(0, math.ceil(fraction * len(sorted_values)) - 1)]


def evaluate(pairs: Sequence[tuple[int, float]], intervals: Iterable[tuple[int, int]],
             start_ns: int | None, end_ns: int | None, *, max_clause: str = "cap") -> dict:
    """Judge pairs and audio intervals over [start_ns, end_ns].

    Coverage, the 5 s window mean and p99 are always judged. The maximum is
    judged by ``max_clause``: "cap" (the drive smoke) is max |skew| <=
    AV_MAX_ABS_MS; "soak_rate" (soak stages) is at most excursion_allowance()
    pairs over AV_SOAK_EXCURSION_MS ("excursions") and none over
    AV_SOAK_HARD_CAP_MS ("hard_cap"). Only pairs whose paint completed inside
    the window count. Returns the signed distribution, coverage, the
    excursions, per-clause results and ``passed``.
    """
    if max_clause not in MAX_CLAUSES:
        raise ValueError(f"unknown max_clause {max_clause!r}")
    if start_ns is None or end_ns is None or end_ns <= start_ns:
        inside: list[tuple[int, float]] = []
        share = 0.0
    else:
        inside = sorted((t, skew) for t, skew in pairs if start_ns <= t <= end_ns)
        share = coverage(intervals, start_ns, end_ns)
    skews = sorted(skew for _, skew in inside)
    magnitudes = sorted(abs(skew) for skew in skews)
    means = window_means(inside, start_ns, end_ns) if inside else None
    worst = max(means, key=abs) if means else None
    stats = {
        "pairs": len(inside), "coverage": share,
        "signed_p1_ms": rank(skews, 0.01) if skews else None,
        "signed_p50_ms": rank(skews, 0.50) if skews else None,
        "signed_p99_ms": rank(skews, 0.99) if skews else None,
        "signed_min_ms": skews[0] if skews else None,
        "signed_max_ms": skews[-1] if skews else None,
        "abs_p99_ms": rank(magnitudes, 0.99) if magnitudes else None,
        "abs_max_ms": magnitudes[-1] if magnitudes else None,
        "window_mean_min_ms": means[0] if means else None,
        "window_mean_max_ms": means[1] if means else None,
        "worst_window_mean_ms": worst,
        "over_50_ms": sum(1 for x in magnitudes if x > 50.0),
    }
    # Excursions: every pair over AV_SOAK_EXCURSION_MS (= AV_MAX_ABS_MS), with
    # its offset into the window; reported for both variants.
    over = [(t, skew) for t, skew in inside if abs(skew) > AV_SOAK_EXCURSION_MS]
    stats["excursions"] = len(over)
    stats["excursion_pairs"] = [{"offset_s": (t - start_ns) / 1e9, "skew_ms": skew}
                                for t, skew in over[:EXCURSIONS_LISTED_MAX]]
    stats["excursion_pairs_truncated"] = len(over) > EXCURSIONS_LISTED_MAX
    stats["over_hard_cap"] = sum(1 for x in magnitudes if x > AV_SOAK_HARD_CAP_MS)
    clauses = {
        "coverage": share >= AV_COVERAGE_MIN,
        "window_mean": worst is not None and abs(worst) <= AV_MEAN_MAX_ABS_MS,
        "p99_abs": bool(magnitudes) and stats["abs_p99_ms"] <= AV_P99_ABS_MAX_MS,
    }
    if max_clause == "cap":
        clauses["max_abs"] = bool(magnitudes) and stats["abs_max_ms"] <= AV_MAX_ABS_MS
    else:
        stats["excursion_allowance"] = excursion_allowance(start_ns, end_ns)
        clauses["excursions"] = bool(magnitudes) and stats["excursions"] <= stats["excursion_allowance"]
        clauses["hard_cap"] = bool(magnitudes) and stats["over_hard_cap"] == 0
    return {"passed": all(clauses.values()), "clauses": clauses, "max_clause": max_clause, **stats}
