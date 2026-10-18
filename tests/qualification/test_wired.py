# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B -m unittest discover -s tests/qualification -v
"""Hermetic package acceptance and independent fail-closed controls."""
from __future__ import annotations

import unittest

from fixture import PackageTest, binding, make_package, rebind, write
from common import Document, EvidenceError, JSON, load
from check_wired import check


class WiredTests(PackageTest):
    def test_complete_synthetic_package_through_both_clis(self) -> None:
        # Given: the explicitly synthetic package created in setUp.
        # When: both shipped CLIs inspect the package.
        results = [self.cli(tool) for tool in ('check_wired.py', 'check_privacy.py')]
        # Then: complete safe evidence satisfies the export contract only.
        for result in results:
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_phone_version_fails_cli(self) -> None:
        # Given: a package without the Android Auto version.
        doc = load(self.manifest)
        doc.child('phone').values.pop('android_auto_version')
        write(self.manifest, doc.values)
        # When: the public CLI checks it.
        result = self.cli('check_wired.py')
        # Then: the incomplete package is rejected.
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_performance_rejected_when_rehashed_task_is_wrong(self) -> None:
        # Given: otherwise valid task32 results bound to their changed bytes.
        for field in ('perf_30m', 'soak_2h'):
            with self.subTest(field=field):
                manifest = make_package(self.root)
                path = self.root / load(manifest).child(field).text('path')
                doc = load(path)
                doc.values['task'] = 'TESTONLY_WRONG_TASK'
                write(path, doc.values)
                rebind(manifest, field)
                # When: the wired CLI validates native result identity.
                result = self.cli('check_wired.py')
                # Then: a matching hash and PASS cannot substitute for task32.
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertNotIn('TESTONLY_WRONG_TASK', result.stdout + result.stderr)

    def test_wrong_metadata_types_fail(self) -> None:
        # Given: independently malformed phone values, never a missing hash.
        for value in (None, True, 15, [], {}, '', 'pending'):
            with self.subTest(value=value):
                manifest = make_package(self.root)
                doc = load(manifest)
                doc.child('phone').values['android_version'] = value
                write(manifest, doc.values)
                # When / Then: strict boundary validation rejects coercion/placeholders.
                with self.assertRaises(EvidenceError):
                    check(manifest)

    def test_pass_labels_do_not_override_result_semantics(self) -> None:
        # Given: each mutation is rehashed, so semantics must cause rejection.
        mutations: tuple[tuple[str, tuple[str, ...], JSON], ...] = (
            ('task30', ('observables', 'audio_present'), False),
            ('task30', ('observables', 'projection_reached'), 1),
            ('task30', ('observables', 'startup_budget_ms'), 15001),
            ('task31', ('subset',), True),
            ('task31', ('budget_ms',), 5001),
            ('perf_30m', ('measurement_origin',), 'offline'),
            ('perf_30m', ('qualification_status',), 'INCOMPLETE'),
            ('perf_30m', ('named_gaps',), ['decode_fps']),
            ('perf_30m', ('provenance_gaps',), ['source_digest']),
            ('perf_30m', ('invalid_telemetry',), ['invalid']),
            ('perf_30m', ('metrics', 'av_sync_ms', 'value'), 51),
            ('perf_30m', ('metrics', 'audio_latency_ms', 'value'), None),
            ('perf_30m', ('metrics', 'audio_latency_ms', 'value'), '200'),
            ('perf_30m', ('metrics', 'rss_growth_pct', 'value'), 10),
            ('perf_30m', ('metrics', 'receiver_rss_peak_mb', 'gated'), False),
            ('perf_30m', ('metrics', 'receiver_rss_peak_mb', 'budget_max'), 999),
            ('perf_30m', ('metrics', 'decode_fps', 'value'), None),
            ('perf_30m', ('run_status', 'active_projection_seconds'), 1799),
            ('perf_30m', ('run_status', 'display_connected'), False),
            ('perf_30m', ('run_status', 'projection_failure'), 'frozen'),
            ('perf_30m', ('crashes',), 1),
            ('perf_30m', ('capture',), {'path': '/private/raw.json', 'sha256': 'a' * 64}),
            ('perf_30m', ('provenance', 'service_definitions'), []),
            ('perf_30m', ('provenance', 'package_digest'), 'f' * 64),
            ('soak_2h', ('duration_seconds',), 7199),
            ('soak_2h', ('run_status', 'active_projection_seconds'), 1800),
            ('soak_2h', ('provenance', 'source_digest'), 'e' * 64),
        )
        for field, keys, value in mutations:
            with self.subTest(field=field, keys=keys):
                manifest = make_package(self.root)
                path = self.root / load(manifest).child(field).text('path')
                result = load(path)
                child = result
                for key in keys[:-1]:
                    child = child.child(key)
                child.values[keys[-1]] = value
                write(path, result.values)
                rebind(manifest, field)
                # When / Then: a forged PASS cannot bypass the independent check.
                with self.assertRaises(EvidenceError):
                    check(manifest)

    def test_recovery_matrix_requires_all_observations_within_deadline(self) -> None:
        # Given: missing/duplicate rows, incomplete evidence, or late terminal evidence.
        for mutation in ('missing', 'duplicate', 'evidence', 'deadline', 'blocked', 'retries'):
            with self.subTest(mutation=mutation):
                manifest = make_package(self.root)
                path = self.root / 'recovery.json'
                result = load(path)
                rows = result.array('scenarios')
                first = Document.parse(rows[0])
                if mutation == 'missing':
                    rows.pop(0)
                elif mutation == 'duplicate':
                    rows.append(rows[0])
                elif mutation == 'evidence':
                    first.values['evidence'] = 'dip+frames'
                elif mutation == 'deadline':
                    first.values.update(recovery_ms=5000, elapsed_ms=5000)
                elif mutation == 'blocked':
                    first.values['status'] = 'BLOCKED'
                else:
                    first.values['retries'] = -1
                write(path, result.values)
                rebind(manifest, 'task31')
                # When / Then: incomplete or late recovery never qualifies.
                with self.assertRaises(EvidenceError):
                    check(manifest)

    def test_hash_mismatch_rejects_modified_log(self) -> None:
        # Given: retained bytes changed after binding.
        (self.root / 'sanitized.log').write_text('different sanitized evidence\n')
        # When / Then: binding failure is distinct from privacy failure.
        with self.assertRaisesRegex(EvidenceError, 'ARTIFACT_SHA256_MISMATCH'):
            check(self.manifest)

    def test_paths_cannot_escape_package(self) -> None:
        # Given: traversal and absolute paths, even with a plausible digest.
        for path in ('../outside.json', '/tmp/outside.json'):
            with self.subTest(path=path):
                doc = load(self.manifest)
                doc.child('task30').values['path'] = path
                write(self.manifest, doc.values)
                # When / Then: no outside file is read.
                with self.assertRaisesRegex(EvidenceError, 'UNSAFE_ARTIFACT_PATH'):
                    check(self.manifest)

    def test_symlink_rejected(self) -> None:
        # Given: a link even to otherwise valid internal evidence.
        (self.root / 'linked.json').symlink_to(self.root / 'projection.json')
        # When / Then: links are excluded from exports.
        self.assertEqual(self.cli('check_wired.py').returncode, 1)

    def test_missing_artifact_fails_cli(self) -> None:
        # Given: a referenced artifact whose bytes are unavailable.
        (self.root / 'projection.json').unlink()
        # When / Then: a recorded digest alone does not qualify.
        self.assertEqual(self.cli('check_wired.py').returncode, 1)

    def test_nonfinite_result_fails_cli(self) -> None:
        # Given: nonstandard JSON containing a non-finite number.
        path = self.root / 'projection.json'
        path.write_text(path.read_text().replace('900', 'NaN'))
        rebind(self.manifest, 'task30')
        # When / Then: invalid telemetry fails without a traceback or raw output.
        self.assertEqual(self.cli('check_wired.py').returncode, 1)

    def test_same_result_cannot_supply_both_perf_windows(self) -> None:
        # Given: a shared result reference for both windows.
        doc = load(self.manifest)
        doc.values['perf_30m'] = binding(self.root / 'soak.json')
        write(self.manifest, doc.values)
        # When / Then: a single long result is not two independent runs.
        with self.assertRaisesRegex(EvidenceError, 'RESULTS_MUST_BE_SEPARATE'):
            check(self.manifest)

    def test_duplicate_json_key_rejected(self) -> None:
        # Given: duplicate manifest keys with ambiguous meaning.
        self.manifest.write_text('{"schema":1,"schema":2}')
        # When / Then: parser fails closed before interpreting fields.
        self.assertEqual(self.cli('check_wired.py').returncode, 1)


if __name__ == '__main__':
    unittest.main()
