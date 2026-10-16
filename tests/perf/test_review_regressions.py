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


if __name__ == "__main__":
    globals()[sys.argv[1]]()
    print(f"PASS review-{sys.argv[1]}")
