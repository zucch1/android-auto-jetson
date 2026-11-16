# SPDX-License-Identifier: GPL-3.0-or-later
# Run: python3 -B -m unittest discover -s tests/qualification -v
"""v3 (signed A/V, moving-map) packages are accepted; v2 history keeps its contract (#137)."""
from __future__ import annotations

import unittest

from fixture import PackageTest, make_package, rebind, write
from common import EvidenceError, load
from check_wired import check


class V3PackageTests(PackageTest):
    def setUp(self) -> None:
        super().setUp()
        self.manifest = make_package(self.root, version=3)

    def tamper(self, field: str, change) -> None:
        name = 'perf.json' if field == 'perf_30m' else 'soak.json'
        doc = load(self.root / name).values
        change(doc)
        write(self.root / name, doc)
        rebind(self.manifest, field)

    def test_v3_package_passes(self) -> None:
        for name in ('perf.json', 'soak.json'):
            self.assertEqual(load(self.root / name).text('schema'), 'aa-projection-perf/3')
        check(self.manifest)
        self.assertEqual(self.cli('check_wired.py').returncode, 0)

    def test_v3_contract_fields_must_agree_and_pass(self) -> None:
        cases = {
            'p99 metric differs from the contract block':
                lambda d: d['metrics']['av_p99_abs_ms'].update(value=11.0),
            'contract not passed': lambda d: d['av_contract'].update(passed=False),
            'a clause failed': lambda d: d['av_contract']['clauses'].update(coverage=False),
            'coverage below the floor': lambda d: d['metrics']['av_coverage_pct'].update(value=80.0),
            'coverage floor weakened': lambda d: d['metrics']['av_coverage_pct'].update(budget_min=50.0),
            'static workload': lambda d: d['workload'].update(kind='none'),
            'av_sync_ms gated again': lambda d: d['metrics']['av_sync_ms'].update(gated=True),
        }
        for label, change in cases.items():
            for field in ('perf_30m', 'soak_2h'):
                with self.subTest(case=label, field=field):
                    self.setUp()
                    self.tamper(field, change)
                    with self.assertRaises(EvidenceError):
                        check(self.manifest)


class V2HistoryTests(PackageTest):
    def test_v2_package_still_passes_under_its_own_contract(self) -> None:
        self.assertEqual(load(self.root / 'soak.json').text('schema'), 'aa-projection-perf/2')
        check(self.manifest)

    def test_v2_av_sync_budget_still_gates_v2(self) -> None:
        doc = load(self.root / 'soak.json').values
        doc['metrics']['av_sync_ms']['value'] = 50.5
        write(self.root / 'soak.json', doc)
        rebind(self.manifest, 'soak_2h')
        with self.assertRaises(EvidenceError):
            check(self.manifest)


if __name__ == '__main__':
    unittest.main()
