#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""PR-56 behavioral regressions; invoked by CTest without live hardware."""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_projection_collect as collector


def av_sync() -> None:
    # Given concurrent scheduled video/audio, with healthy audio but video 90 ms late.
    parser = collector.EventParser()
    events = [
        {"event": "media_timing", "av_sync_ms": 2, "audio_scheduling_skew_ms": 2},
        {"event": "audio_write_completed", "scheduled_ns": 1_000_000_000,
         "scheduled_end_ns": 1_020_000_000, "completed_ns": 1_002_000_000},
        {"event": "video_presented", "frame": 9, "scheduled_ns": 1_000_000_000,
         "completed_ns": 1_092_000_000},
    ]
    # When both endpoint event streams are consumed.
    for event in events:
        parser._consume(json.dumps(event))
    # Then video lag cannot be hidden by the old audio-only number.
    assert parser.metrics["av_sync_ms"] == 90, parser.metrics
    assert parser.metrics["audio_scheduling_skew_ms"] == 2
    # Given reversed log arrival and a healed later sample.
    for event in reversed(events[1:]):
        parser._consume(json.dumps(event))
    parser._consume(json.dumps({"event": "video_presented", "frame": 10,
                               "scheduled_ns": 1_010_000_000,
                               "completed_ns": 1_003_000_000}))
    assert parser.metrics["av_sync_ms"] == 90, parser.metrics
    # An unpaired video, or a legacy proxy alone, cannot supply a measurement.
    unpaired = collector.EventParser()
    unpaired._consume(json.dumps(events[0]))
    unpaired._consume(json.dumps(events[2]))
    assert unpaired.metrics["av_sync_ms"] is None
    # Given a frame near the end of a 42 ms audio chunk: video takes 30 ms, audio 2 ms.
    chunk = collector.EventParser()
    chunk._consume(json.dumps({"event": "audio_write_completed", "scheduled_ns": 3_000_000_000,
                               "scheduled_end_ns": 3_042_000_000,
                               "completed_ns": 3_002_000_000}))
    chunk._consume(json.dumps({"event": "video_presented", "frame": 11,
                               "scheduled_ns": 3_040_000_000,
                               "completed_ns": 3_070_000_000}))
    # Then skew is the 28 ms latency difference, not the 68 ms completion gap.
    assert chunk.metrics["av_sync_ms"] == 28, chunk.metrics

    # #60: given a sink that commits audio 30 ms before it plays (pulsesink ring
    # buffer), presented_ns, not the handoff, is the audio endpoint.
    early = collector.EventParser()
    early._consume(json.dumps({"event": "audio_write_completed", "scheduled_ns": 5_000_000_000,
                               "scheduled_end_ns": 5_020_000_000,
                               "completed_ns": 4_970_000_000,
                               "presented_ns": 5_001_000_000}))
    early._consume(json.dumps({"event": "video_presented", "frame": 12,
                               "scheduled_ns": 5_000_000_000,
                               "completed_ns": 5_011_000_000}))
    # Then skew is 11 - 1 = 10 ms, not the 41 ms the early handoff would give.
    assert early.metrics["av_sync_ms"] == 10, early.metrics
    # A presented time before the handoff, or a non-integer one, is invalid
    # telemetry, never silently replaced by the handoff.
    for bad in (4_960_000_000, 5.0e9, None, True):
        broken = collector.EventParser()
        broken._consume(json.dumps({"event": "audio_write_completed",
                                    "scheduled_ns": 5_000_000_000,
                                    "scheduled_end_ns": 5_020_000_000,
                                    "completed_ns": 4_970_000_000, "presented_ns": bad}))
        assert broken.metrics["av_sync_ms"] is None, bad
        assert "av_sync_ms" in broken.invalid_telemetry, bad


def display_sources() -> None:
    source = (ROOT / "tools/perf/aa-projection-perf.sh").read_text()
    command = re.search(r'display_status="\$\(remote "(.*)" 2>/dev/null', source).group(1)
    with tempfile.TemporaryDirectory(prefix="perf-display-") as tmp:
        base = Path(tmp)
        connector = base / "drm/card0-DP-1"
        connector.mkdir(parents=True)
        for name in ("cat", "grep"):
            (base / name).symlink_to(Path("/usr/bin") / name)
        command = command.replace("/sys/class/drm", str(base / "drm"))
        script = 'set -euo pipefail; command="' + command + '"; eval "$command"'
        for drm, randr, ready in (("disconnected", "DP-0 connected 1920x1080+0+0", True),
                                  ("disconnected", "DP-0 disconnected", False),
                                  ("connected", None, True),
                                  ("unknown", "DP-0 disconnected", False)):
            # Given real shell probes with isolated connector files and session command.
            (connector / "status").write_text(drm + "\n")
            xrandr = base / "xrandr"
            if randr is None:
                xrandr.unlink(missing_ok=True)
            else:
                xrandr.write_text("#!/bin/bash\nset -euo pipefail\nprintf '%s\\n' \"$RANDR\"\n")
                xrandr.chmod(0o755)
            # When the readiness expression queries both platform sources.
            proc = subprocess.run(["/bin/bash", "-c", script], capture_output=True, text=True,
                                  env=dict(os.environ, PATH=str(base), RANDR=randr or ""), timeout=5)
            # Then only positive connected evidence makes it ready.
            actual = "connected" in proc.stdout.splitlines()
            assert actual == ready, (drm, randr, proc.stdout, proc.stderr)


def stall_dropouts() -> None:
    # Given two survived sink stalls (#60), each logged at begin and end.
    parser = collector.EventParser()
    for dropped, discarded in ((5, 2), (1, 0)):
        parser._consume(json.dumps({"event": "audio_stall_bypass", "phase": "begin",
                                    "trigger": "credit_wait", "discarded": discarded}))
        parser._consume(json.dumps({"event": "audio_stall_bypass", "phase": "end",
                                    "outcome": "write_returned", "dropped": dropped,
                                    "discarded": discarded}))
    # Then each stall is one reported dropout and every lost chunk is counted.
    assert parser.metrics["audio_stall_dropouts"] == 2, parser.metrics
    assert parser.metrics["audio_stall_dropped_chunks"] == 8, parser.metrics
    assert parser.metrics["audio_stall_unrecovered"] == 0, parser.metrics
    # And survived stalls are not terminal errors.
    assert parser.terminal_errors == [], parser.terminal_errors
    # Given a failed reopen, a stop during recovery and a bypass that never ends.
    for outcome in ("reopen_failed", "stopped"):
        parser._consume(json.dumps({"event": "audio_stall_bypass", "phase": "begin",
                                    "trigger": "sink_write_error", "discarded": 1}))
        parser._consume(json.dumps({"event": "audio_stall_bypass", "phase": "end",
                                    "outcome": outcome, "dropped": 2, "discarded": 1,
                                    "partial": 1, "rendered": 1}))
    parser._consume(json.dumps({"event": "audio_stall_bypass", "phase": "begin",
                                "trigger": "credit_wait", "discarded": 4}))
    # Then none of them counts as survived, all three are unrecovered, and
    # only ended stalls contribute measured lost chunks: a partly cut chunk is
    # lost, a fully rendered in-flight chunk is not.
    assert parser.metrics["audio_stall_dropouts"] == 2, parser.metrics
    assert parser.metrics["audio_stall_unrecovered"] == 3, parser.metrics
    assert parser.metrics["audio_stall_dropped_chunks"] == 16, parser.metrics


if __name__ == "__main__":
    globals()[sys.argv[1]]()
    print(f"PASS review-{sys.argv[1]}")
