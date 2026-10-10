# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed receipt-name routes for the local stage1 driver; never a remote module.

One table maps the exact `--receipt <path>` spelling onto one fixed route kind.
No arbitrary destinations, no options and no extra argv are accepted. The
default route (no arguments) is reachable only without arguments, exactly as
before. Historical receipts stay untouched: reservation is exclusive-open.
"""
from __future__ import annotations

from pathlib import Path
from typing import Final, Literal

RouteKind = Literal["expanded", "ceiling", "target", "sizing"]

# Exact fixed names only. The default name is NOT explicitly argv-addressable.
DEFAULT_NAME: Final = "task-5-prerequisites-expanded.json"
EXPANDED: Final = ("task-5-prerequisites-namespace.json",
                   "task-5-prerequisites-capacity.json")
CEILING: Final = "task-5-prerequisites-ceiling.json"
TARGET: Final = ("task-5-prerequisites-ceiling-attempt.json",
                 "task-5-prerequisites-ceiling-retry.json",
                 "task-5-prerequisites-ceiling-policy.json",
                 "task-5-prerequisites-ceiling-policy-resumed.json",
                 "task-5-prerequisites-ceiling-policy-durable-resume.json",
                 "task-5-prerequisites-ceiling-capacity250k.json",
                 "task-5-prerequisites-ceiling-two-links.json")
SIZING: Final = "task-5-topology-sizing.json"
SIZING_3GIB: Final = "task-5-topology-sizing-3gib.json"
SIZING_RSS_LIFECYCLE: Final = "task-5-topology-sizing-rss-lifecycle.json"
SIZING_SQLITE_ABSENCE: Final = "task-5-topology-sizing-sqlite-absence.json"


class ReceiptDestinationError(RuntimeError):
    """The explicit destination is outside the single authorized receipt."""

    def __init__(self, arguments: list[str]) -> None:
        self.arguments = arguments
        super().__init__(arguments)


def _table() -> tuple[tuple[str, RouteKind], ...]:
    return (*((name, "expanded") for name in EXPANDED),
            (CEILING, "ceiling"),
            *((name, "target") for name in TARGET),
            (SIZING, "sizing"),
            (SIZING_3GIB, "sizing"),
            (SIZING_RSS_LIFECYCLE, "sizing"),
            (SIZING_SQLITE_ABSENCE, "sizing"))


def resolve(evidence: Path, arguments: list[str]) -> tuple[Path, RouteKind]:
    """Select exactly one fixed route from exact argv; reject every other shape."""
    if not arguments:
        return evidence / DEFAULT_NAME, "expanded"
    selected = [(evidence / name, kind) for name, kind in _table()
                if arguments == ["--receipt", str(evidence / name)]]
    if len(selected) != 1:
        raise ReceiptDestinationError(arguments)
    return selected[0]
