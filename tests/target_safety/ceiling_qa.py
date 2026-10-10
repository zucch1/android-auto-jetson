# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_qa.py
"""Local QA entrypoint for the ceiling stdin supervisor lifecycle. No remote action.

Runs the guard stdin-lifecycle unit tests and the actual supervisor_bundle stdin
integration (fake setting + real Python stdin guard). Parent QA command:
    python3 -B tests/target_safety/ceiling_qa.py
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    for suite in ("scenarios", "ceiling", "ceiling_guard", "ceiling_window", "ceiling_main",
                  "ceiling_attempt", "ceiling_stdin", "ceiling_bundle", "ceiling_defects",
                  "ceiling_production", "ceiling_budget", "ceiling_argv", "ceiling_policy", "ceiling_durable",
                  "ceiling_capture_paths", "ceiling_capacity_route", "ceiling_two_links_route",
                  "sizing_outcomes", "sizing_resources", "sizing_reading_validation", "sizing_collector",
                  "sizing_selector_failures", "sizing_bundle", "sizing_route", "sizing_admission",
                  "sizing_failure_paths", "sizing_callback_timing", "sizing_completion_integrity",
                  "sizing_rss_lifecycle", "sizing_sqlite_route", "sizing_sqlite_absence",
                  "sizing_sqlite_wire", "sizing_sqlite_inotify", "sizing_sqlite_stdin",
                  "diag_writer", "diag_heartbeat", "diag_stream", "diag_frames"):
        result = subprocess.run((sys.executable, "-B", f"tests/target_safety/{suite}.py"),
                                cwd=root, check=False, timeout=120)
        if result.returncode:
            return result.returncode
    print("PASS all local ceiling qualification suites; no target contact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
