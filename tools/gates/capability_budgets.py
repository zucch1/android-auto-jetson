# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Fixed capability-gate budget table.

Limits come from the plan "Contract budgets" and "Decode pass rule" lines. A
report may never redefine a limit, kind, unit, area or availability stage: any
declared deviation is a budget-weakening attempt and fails the gate.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Final


class Kind(StrEnum):
    MAX = 'max'
    MIN = 'min'


class Stage(StrEnum):
    PROBE = 'probe'
    RELEASE = 'release'


@dataclass(frozen=True, slots=True)
class FixedBudget:
    id: str
    area: str
    kind: Kind
    limit: float
    unit: str
    available_at: Stage

    def within(self, measured: float) -> bool:
        if self.kind is Kind.MAX:
            return measured <= self.limit
        return measured >= self.limit


# Probe-stage budgets: measurable by the early capability probes (decode pass rule).
# Release-stage budgets: audio, reconnect, recovery and soak budgets that stay
# provisional until later qualification; grouped under the capability area that
# owns the eventual measurement.
FIXED_BUDGETS: Final = (
    FixedBudget('decode.duration_s', 'decode', Kind.MIN, 600.0, 's', Stage.PROBE),
    FixedBudget('decode.fps_error_percent', 'decode', Kind.MAX, 5.0, 'percent', Stage.PROBE),
    FixedBudget('decode.bitrate_error_percent', 'decode', Kind.MAX, 5.0, 'percent', Stage.PROBE),
    FixedBudget('decode.dropped_frames', 'decode', Kind.MAX, 0.0, 'frames', Stage.PROBE),
    FixedBudget('decode.p95_latency_ms', 'decode', Kind.MAX, 33.0, 'ms', Stage.PROBE),
    FixedBudget('decode.cpu_percent_one_core', 'decode', Kind.MAX, 50.0, 'percent', Stage.PROBE),
    FixedBudget('audio.path_latency_ms', 'decode', Kind.MAX, 200.0, 'ms', Stage.RELEASE),
    FixedBudget('media.avsync_ms', 'decode', Kind.MAX, 50.0, 'ms', Stage.RELEASE),
    FixedBudget('ipc.duration_s', 'ipc', Kind.MIN, 600.0, 's', Stage.PROBE),
    FixedBudget('ipc.throughput_mbps', 'ipc', Kind.MIN, 10.0, 'Mbps', Stage.PROBE),
    FixedBudget('ipc.p95_added_latency_ms', 'ipc', Kind.MAX, 33.0, 'ms', Stage.PROBE),
    FixedBudget('ipc.happy_drops', 'ipc', Kind.MAX, 0.0, 'frames', Stage.PROBE),
    FixedBudget('session.projection_recovery_s', 'ipc', Kind.MAX, 60.0, 's', Stage.RELEASE),
    FixedBudget('soak.duration_h', 'ipc', Kind.MIN, 4.0, 'h', Stage.RELEASE),
    FixedBudget('soak.crashes', 'ipc', Kind.MAX, 0.0, 'events', Stage.RELEASE),
    FixedBudget('soak.rss_growth_percent', 'ipc', Kind.MAX, 10.0, 'percent', Stage.RELEASE),
    FixedBudget('rss.receiver_mb', 'ipc', Kind.MAX, 250.0, 'MB', Stage.RELEASE),
    FixedBudget('rss.helper_mb', 'ipc', Kind.MAX, 32.0, 'MB', Stage.RELEASE),
    FixedBudget('receiver.cpu_sustained_percent_one_core', 'ipc', Kind.MAX, 50.0, 'percent', Stage.RELEASE),
    FixedBudget('usb.session_reconnect_s', 'usb', Kind.MAX, 5.0, 's', Stage.RELEASE),
    FixedBudget('wireless.session_reconnect_s', 'wireless', Kind.MAX, 15.0, 's', Stage.RELEASE),
    FixedBudget('wireless.ap_startup_s', 'wireless', Kind.MAX, 5.0, 's', Stage.RELEASE),
)

FIXED_BY_ID: Final = {budget.id: budget for budget in FIXED_BUDGETS}


def available(budget: FixedBudget, stage: Stage) -> bool:
    """True when the budget must be measured at the given enforcement stage."""
    if stage is Stage.RELEASE:
        return True
    return budget.available_at is Stage.PROBE
