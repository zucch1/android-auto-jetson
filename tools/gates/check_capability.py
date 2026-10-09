# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/gates/check_capability.py docs/capability-gate.json [--stage continuation|final-release]
"""Fail-fast capability gate over a self-contained capability report.

Exit 0: every area is full-pass / probe-pass-provisional / fallback-probe-pass
        and every budget available at the run stage is measured and passing.
Exit 2: continuation only with owner-signed-reduced-scope areas that carry a
        valid owner sign-off naming the withdrawn claims; never full compliance.
Exit 1: blockers, missing areas, invalid sign-offs, weakened budgets, unmeasured
        budgets falsely claimed as passed, unsigned budget misses, or unresolved
        interim classes at the final-release stage.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Final

from capability_budgets import FIXED_BY_ID, Stage, available
from capability_model import (
    BudgetView,
    Classification,
    GateError,
    Report,
    RunStage,
)
from capability_parse import load

STAGE_OF: Final = {RunStage.CONTINUATION: Stage.PROBE, RunStage.FINAL_RELEASE: Stage.RELEASE}


def evaluate(report: Report, stage: str) -> list[GateError]:
    errors: list[GateError] = []
    for area in report.areas:
        prefix = f'{area.id}.budgets'
        if area.classification == Classification.BLOCKER:
            errors.append(GateError('blocker-present', f'report.areas.{area.id}'))
        # full-pass claims every owned budget passing, so its release-stage budgets
        # are enforced even during continuation runs.
        require_all = area.classification == Classification.FULL_PASS
        for budget in area.budgets:
            now = require_all or available(FIXED_BY_ID[budget.id], STAGE_OF[stage])
            errors.extend(_budget_rules(budget, now, prefix))
        if stage == RunStage.FINAL_RELEASE and area.classification in Classification.INTERIM:
            errors.append(GateError('provisional-unresolved', f'report.areas.{area.id}'))
    return errors


def _budget_rules(budget: BudgetView, available_now: bool, prefix: str) -> list[GateError]:
    field = f'{prefix}.{budget.id}'
    errors: list[GateError] = []
    measured = budget.measured
    verdict = budget.passed if measured is None else FIXED_BY_ID[budget.id].within(measured)
    if budget.passed and measured is None:
        errors.append(GateError('budget-unmeasured-pass', field))
    elif measured is not None and budget.passed != verdict:
        errors.append(GateError('budget-verdict-mismatch', field))
    if budget.withdrawn or not available_now:
        return errors
    if measured is None:
        errors.append(GateError('budget-unmeasured', field))
    elif not verdict:
        errors.append(GateError('budget-fail-unsigned', field))
    return errors


def outcome(report: Report, errors: list[GateError]) -> int:
    if errors:
        return 1
    reduced = any(a.classification == Classification.OWNER_SIGNED_REDUCED_SCOPE for a in report.areas)
    return 2 if reduced else 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--stage', choices=RunStage.ALL, default=None,
                        help='enforcement stage (default: the report\'s declared stage)')
    args = parser.parse_args(argv)
    try:
        report = load(args.report)
    except GateError as exc:
        sys.stderr.write(f'{exc}\n')
        sys.stdout.write('capability-gate: RESULT exit=1 (invalid report)\n')
        return 1
    stage = args.stage or report.report_stage
    errors = evaluate(report, stage)
    for exc in errors:
        sys.stderr.write(f'{exc}\n')
    code = outcome(report, errors)
    counts = {name: sum(1 for a in report.areas if a.classification == name) for name in Classification.ALL}
    sys.stdout.write(f'capability-gate: stage={stage} areas={len(report.areas)} '
                     f'budgets={sum(len(a.budgets) for a in report.areas)} errors={len(errors)}\n')
    sys.stdout.write('capability-gate: classes ' + ' '.join(f'{name}={counts[name]}' for name in Classification.ALL) + '\n')
    label = {0: 'all available budgets pass', 1: 'gate failure', 2: 'owner-signed-reduced-scope continuation'}[code]
    sys.stdout.write(f'capability-gate: RESULT exit={code} ({label})\n')
    return code


if __name__ == '__main__':
    sys.exit(main())
