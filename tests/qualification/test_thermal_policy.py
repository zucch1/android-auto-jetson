# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic Nano scoped exports; never hardware qualification evidence."""
from __future__ import annotations

import copy
import sys
import unittest
from typing import assert_never

from fixture import ROOT, PackageTest, rebind, write
from common import Document, EvidenceError, load
from check_wired import check
from results import performance
from aa_perf_report import build_result

sys.path.insert(0, str(ROOT / 'tests/perf'))
from test_v2_thermal_policy import MODEL, PROFILE, nano_capture


class ThermalPolicyTests(PackageTest):
    def install_nano_results(self) -> None:
        manifest = load(self.manifest)
        manifest.child('target').values['model'] = MODEL
        manifest.child('provenance').child('target_hardware_state').values['model'] = MODEL
        write(self.manifest, manifest.values)
        for field, filename, seconds, now in (
                ('perf_30m', 'perf.json', 1800, '2026-01-01T00:30:00Z'),
                ('soak_2h', 'soak.json', 7200, '2026-01-01T02:30:00Z')):
            raw = nano_capture(seconds)
            raw['measurement_origin'] = 'live'
            raw['provenance'] = copy.deepcopy(manifest.child('provenance').values)
            result = build_result(raw, duration_requested=None, now=now,
                                  capture_path=None, capture_sha256=None)
            write(self.root / filename, result)
            rebind(self.manifest, field)

    def test_scoped_results_accept_without_private_capture(self) -> None:
        # Given independently reported synthetic 30m and 2h Nano evidence.
        self.install_nano_results()
        # When the real sanitized-package validator checks it.
        process = self.cli('check_wired.py')
        # Then only the explicitly disclosed scope passes, with all raw CV identities retained.
        self.assertEqual(process.returncode, 0, process.stdout)
        result = load(self.root / 'perf.json')
        self.assertNotIn('capture', result.values)
        scope = result.child('measurement_coverage').child('thermal_scope')
        self.assertEqual(scope.child('policy').text('profile'), PROFILE)
        self.assertEqual(len(scope.array('zones')), 9)

    def test_rehashed_forged_scopes_reject_after_capture_removal(self) -> None:
        # Given real-reporter results but independently forged summary metadata or readings.
        self.install_nano_results()
        original = load(self.root / 'perf.json').values
        for fault in ('strict', 'unknown', 'model', 'null_cpu', 'missing_cv', 'required_count'):
            with self.subTest(fault=fault):
                result = Document.parse(copy.deepcopy(original))
                coverage = result.child('measurement_coverage')
                scope = coverage.child('thermal_scope')
                match fault:
                    case 'strict':
                        coverage.values['policy'] = 'active-window-coverage/1'
                        coverage.values.pop('thermal_scope')
                    case 'unknown': scope.child('policy').values['profile'] = 'unknown'
                    case 'model':
                        result.child('provenance').child('target_hardware_state').values['model'] = 'other Jetson'
                    case 'null_cpu':
                        Document.parse(result.child('thermal_state').array('zones')[0]).values['temp_c'] = None
                    case 'missing_cv': scope.array('zones').pop(2)
                    case 'required_count':
                        zone = Document.parse(scope.array('zones')[0])
                        zone.values['numeric'] = 360
                        zone.values['unavailable'] = 1
                    case unreachable:
                        assert_never(unreachable)
                # When / Then: both semantic parsing and rehashed package validation reject.
                with self.assertRaises(EvidenceError):
                    performance(result, 1800)
                write(self.root / 'perf.json', result.values)
                rebind(self.manifest, 'perf_30m')
                with self.assertRaises(EvidenceError):
                    check(self.manifest)

    def test_manifest_model_must_agree(self) -> None:
        # Given scoped results bound to an approved model in their own provenance.
        self.install_nano_results()
        manifest = load(self.manifest)
        manifest.child('target').values['model'] = 'other Jetson'
        write(self.manifest, manifest.values)
        # When / Then: matching artifact hashes do not authorize another manifest target.
        with self.assertRaises(EvidenceError):
            check(self.manifest)

    def test_two_run_policies_must_agree(self) -> None:
        # Given an individually valid strict soak next to a scoped Nano performance run.
        self.install_nano_results()
        raw = nano_capture(7200)
        raw.pop('thermal_policy')
        raw['measurement_origin'] = 'live'
        raw['provenance'] = load(self.manifest).child('provenance').values
        for sample in raw['samples']:
            for zone in sample['thermal_zones']:
                zone['temp_c'] = 45.0
        result = build_result(raw, duration_requested=None, now='2026-01-01T02:30:00Z',
                              capture_path=None, capture_sha256=None)
        write(self.root / 'soak.json', result)
        rebind(self.manifest, 'soak_2h')
        # When / Then: a package cannot silently mix strict and scoped run contracts.
        with self.assertRaises(EvidenceError):
            check(self.manifest)


if __name__ == '__main__':
    unittest.main()
