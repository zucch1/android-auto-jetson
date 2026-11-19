# SPDX-License-Identifier: GPL-3.0-or-later
"""Log-integrity evidence shared by the qualification gates (#90 review).

The receiver never blocks a media, audio or control thread on a stalled stdout,
so under backpressure it may drop or delay records. Gates must therefore prove a
log complete before trusting what it does not contain:

* ``log_complete`` is the receiver logger's last line, written only after a
  clean drain. It carries cumulative loss counters and the worst submit-to-write
  delay. Its absence (logger timeout, crash, kill, truncated read) means the
  log is incomplete.
* ``runtime_log_dropped`` / ``media_telemetry_dropped`` are periodic summaries;
  a malformed count is treated as loss.
* ``telemetry_complete`` is the display telemetry writer's last line.

A completion record proves a log complete only when it is the single terminal
record: a second one, or any record after it, is incomplete (#90 review 4).
Every flag fails closed: an incomplete log reports every loss class as possible.
"""
from __future__ import annotations

from collections.abc import Iterable
from typing import Any

# Worst stdout delivery delay beyond which arrival-timed evidence (a harness that
# polls the log and times what it sees) is inconclusive.
LATE_DELIVERY_MS = 100

_COMPLETE_FIELDS = ("runtime_dropped", "state_overflow", "media_rate_limited",
                    "media_backpressure", "shutdown_dropped", "max_delivery_lag_ms")
RELEVANT_EVENTS = frozenset({"log_complete", "runtime_log_dropped", "media_telemetry_dropped",
                             "telemetry_complete"})
# Stand-in for "some record followed the completion record", for consumers
# that keep only RELEVANT_EVENTS (the collector).
AFTER_COMPLETION = "record_after_completion"


def _count(value: Any) -> int | None:
    """A non-negative integer count, or None when absent or malformed."""
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return value


def assess_receiver(events: Iterable[dict]) -> dict:
    """Integrity of one receiver log (one process generation)."""
    complete: dict | None = None
    malformed_complete = False
    completions = 0
    after_completion = False
    state_lost = media_lost = runtime_lost = False
    for event in events:
        if not isinstance(event, dict):
            continue
        kind = event.get("event")
        if completions and kind != "log_complete":
            after_completion = True  # the completion record was not terminal
        if kind == "log_complete":
            completions += 1
            counts = {key: _count(event.get(key)) for key in _COMPLETE_FIELDS}
            if any(value is None for value in counts.values()):
                malformed_complete = True
            else:
                complete = counts
        elif kind == "runtime_log_dropped":
            overflow, pressure = _count(event.get("state_overflow")), _count(event.get("backpressure"))
            if overflow != 0:
                state_lost = True
            if pressure is None or pressure - (overflow or 0) > 0:
                runtime_lost = True
        elif kind == "media_telemetry_dropped":
            if _count(event.get("rate_limited")) != 0 or _count(event.get("backpressure")) != 0:
                media_lost = True
    is_complete = (complete is not None and not malformed_complete and completions == 1
                   and not after_completion)
    if is_complete:
        assert complete is not None
        state_lost = state_lost or complete["state_overflow"] > 0 or complete["shutdown_dropped"] > 0
        media_lost = media_lost or complete["media_rate_limited"] + complete["media_backpressure"] > 0
        runtime_lost = runtime_lost or complete["runtime_dropped"] - complete["state_overflow"] > 0
    else:
        state_lost = media_lost = runtime_lost = True
    lag = complete["max_delivery_lag_ms"] if is_complete and complete else None
    return {
        "complete": is_complete,
        "state_lost": state_lost,
        "media_lost": media_lost,
        "runtime_lost": runtime_lost,
        "max_delivery_lag_ms": lag,
        # Arrival-timed evidence is only conclusive when delivery was prompt.
        "late_delivery": lag is None or lag > LATE_DELIVERY_MS,
    }


def assess_display(events: Iterable[dict]) -> dict:
    """Integrity of one display telemetry log."""
    records = [e for e in events if isinstance(e, dict)]
    complete = (bool(records) and records[-1].get("event") == "telemetry_complete"
                and sum(1 for e in records if e.get("event") == "telemetry_complete") == 1)
    return {"complete": complete}


def emit_ns(event: dict) -> int | None:
    """The record's source emission time (CLOCK_MONOTONIC ns), when stamped."""
    value = event.get("emit_ns")
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        return None
    return value


def assess_generations(text: str) -> dict:
    """Aggregate integrity over several receiver logs read in one stream.

    Each log starts with a ``### <name>`` marker line, followed by its integrity
    records, then ``### last`` and the log's last JSON record (framing that
    proves the completion record terminal). ``### unreadable`` or ``### missing``
    marks a log that could not be read or was not there. Any unreadable,
    missing, unframed or incomplete generation makes the whole set incomplete.
    """
    import json

    generations: list[dict] = []
    unreadable = False
    for raw in text.splitlines():
        if raw.startswith("### "):
            marker = raw.strip()
            if marker in ("### unreadable", "### missing"):
                unreadable = True
                if generations:
                    generations[-1]["lost"] = True
            elif marker == "### last":
                if generations:
                    generations[-1]["framed"] = True
            else:
                generations.append({"name": marker[4:].strip(), "events": [], "framed": False,
                                    "last": None, "after_last": 0})
            continue
        if not generations or not raw.strip():
            continue
        try:
            event = json.loads(raw)
        except json.JSONDecodeError:
            event = None
        current = generations[-1]
        if current["framed"]:
            # Exactly one line follows "### last" (#90 review 5): count lines,
            # so a malformed one cannot be replaced by a later valid record.
            current["after_last"] += 1
            current["last"] = event if current["after_last"] == 1 else False
        elif isinstance(event, dict):
            current["events"].append(event)
    results = []
    names = []
    for current in generations:
        names.append(current["name"])
        result = assess_receiver(current["events"])
        completions = [e for e in current["events"] if e.get("event") == "log_complete"]
        terminal = (current["framed"] and isinstance(current["last"], dict)
                    and len(completions) == 1 and current["last"] == completions[0])
        if not terminal or current.get("lost"):
            result = assess_receiver([])  # completion not proven terminal: incomplete
        results.append(result)
    complete = bool(results) and not unreadable and all(r["complete"] for r in results)
    return {
        "per_generation": dict(zip(names, results)),
        "generations": len(results),
        "complete": complete,
        "state_lost": not complete or any(r["state_lost"] for r in results),
        "media_lost": not complete or any(r["media_lost"] for r in results),
        "runtime_lost": not complete or any(r["runtime_lost"] for r in results),
        "late_delivery": not complete or any(r["late_delivery"] for r in results),
    }


def main(argv: list[str]) -> int:
    """``generations``: read a marked log stream on stdin, print shell-safe flags."""
    import sys

    if argv != ["generations"]:
        print("usage: aa_log_integrity.py generations < stream", file=sys.stderr)
        return 2
    result = assess_generations(sys.stdin.read())
    flags = ("complete", "state_lost", "media_lost", "runtime_lost", "late_delivery")
    print(" ".join(f"{key}={int(result[key])}" for key in ("generations", *flags)))
    # Then one line per log, so a gate can scope a verdict to the logs it used.
    for name, one in result["per_generation"].items():
        if name and " " not in name and "=" not in name:
            print(f"gen={name} " + " ".join(f"{key}={int(one[key])}" for key in flags))
    return 0


if __name__ == "__main__":
    import sys

    sys.exit(main(sys.argv[1:]))
