# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared helpers for capability gate CLI tests."""
from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / 'tools' / 'gates' / 'check_capability.py'
GATE_MODULES = ('capability_budgets.py', 'capability_model.py', 'capability_parse.py', 'check_capability.py')
FIXTURES = ROOT / 'tests' / 'gates' / 'fixtures'
REPORT = ROOT / 'docs' / 'capability-gate.json'


def run_gate(path: Path, stage: str | None = None) -> tuple[int, str]:
    argv = [sys.executable, '-B', str(GATE), str(path)]
    if stage is not None:
        argv += ['--stage', stage]
    result = subprocess.run(argv, capture_output=True, text=True, check=False)
    return result.returncode, result.stdout + result.stderr


class GateCase(unittest.TestCase):
    def assert_exit(self, report: dict, expected: int, code: str | None = None,
                    stage: str | None = None, base: Path | None = None) -> None:
        source = base if base is not None else FIXTURES / 'good_provisional.json'
        data = copy.deepcopy(report) if report else json.loads(source.read_text(encoding='utf-8'))
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as tmp:
            path = Path(tmp) / 'report.json'
            path.write_text(json.dumps(data, allow_nan=False) if not isinstance(report, Path)
                            else report.read_text(encoding='utf-8'), encoding='utf-8')
            if isinstance(report, Path):
                path = report
            exit_code, output = run_gate(path, stage)
        self.assertEqual(exit_code, expected, msg=output)
        if code is not None:
            self.assertIn(code, output)

    def assert_raw_exit(self, payload: bytes, expected: int, code: str) -> None:
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as tmp:
            path = Path(tmp) / 'report.json'
            path.write_bytes(payload)
            exit_code, output = run_gate(path)
        self.assertEqual(exit_code, expected, msg=output)
        self.assertIn(code, output)

    @staticmethod
    def mutate(base: dict, area_id: str, budget_id: str | None = None) -> dict:
        """Return a live reference into base so caller mutations take effect."""
        for area in base['areas']:
            if area['id'] == area_id:
                if budget_id is None:
                    return area
                for budget in area['budgets']:
                    if budget['id'] == budget_id:
                        return budget
        raise AssertionError(f'fixture has no {area_id}/{budget_id}')
