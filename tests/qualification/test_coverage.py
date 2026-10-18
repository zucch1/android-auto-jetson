# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent sanitized-summary acceptance; all inputs are synthetic."""
from __future__ import annotations

import copy
import json
import sys
import unittest

from fixture import ROOT, PackageTest, rebind, write
from common import Document, EvidenceError, JSON, load
from check_wired import check
from results import performance
from aa_perf_report import build_result
from synthetic_capture import capture

sys.path.insert(0, str(ROOT / 'tests/perf'))
from test_v2_coverage import synthetic, reporter_cli


class CoverageTests(PackageTest):
    def test_ipc_receive_subset_rejects_without_raw_capture(self) -> None:
        for edge in ('first_ns', 'last_ns'):
            with self.subTest(edge=edge):
                # Given the sanitized summary of balanced counts but unmatched receive bounds.
                raw = synthetic(1800)
                raw['measurement_origin'] = 'live'
                raw['provenance']['target_hardware_state'] = {'model': 'synthetic'}
                doc = build_result(raw, duration_requested=None, now='2026-01-01T00:30:00Z',
                                   capture_path=None, capture_sha256=None)
                consumed = doc['measurement_coverage']['ipc_consumption']
                consumed[edge] += 5_000_000_000 if edge == 'first_ns' else -5_000_000_000
                if edge == 'first_ns':
                    consumed.update(begin_ns=5_000_000_000, max_gap_ns=10_000_000_000)
                doc['metrics']['ipc_throughput_mbps']['value'] = (
                    consumed['bytes'] * 8000 / (consumed['last_ns'] - consumed['begin_ns']))
                # When / Then: independent task33 parsing rejects the receive subset contradiction.
                with self.assertRaises(EvidenceError):
                    performance(Document.parse(doc), 1800)

    def test_consistent_four_second_decode_accepts_sanitized_cli_result(self) -> None:
        # Given genuine four-second decode spans in the Oracle's synthetic capture.
        raw = synthetic(1800)
        raw['provenance']['target_hardware_state'] = {'model': 'synthetic'}
        for block in raw['video_evidence']['blocks']:
            block['decode']['begin_ns'] = block['decode']['last_ns'] - 4_000_000_000
        raw['video_evidence']['decode_latency_ns'] = [4_000_000_000] * 360
        process, output = reporter_cli(json.dumps(raw))
        self.assertEqual(process.returncode, 0, process.stderr)
        doc = json.loads(output)
        doc.pop('capture')
        # When: task33 consumes the sanitized real CLI output.
        performance(Document.parse(doc), 1800)
        # Then: latency is still informational, not a reinstated 33ms budget.
        self.assertEqual(doc['metrics']['decode_p95_ms']['value'], 4000)

    def test_exact_ten_second_coverage_gaps_accept(self) -> None:
        # Given: consistent point summaries at the inclusive coverage limit.
        result = load(self.root / 'perf.json')
        coverage = result.child('measurement_coverage')
        for name in ('sampling', 'rss', 'thermal', 'ipc_consumption', 'ipc_matches', 'decode'):
            coverage.child(name).values['max_gap_ns'] = 10_000_000_000
        # When: independent acceptance evaluates the availability boundary.
        performance(result, 1800)
        # Then: the unchanged PASS is admissible without a tighter availability budget.
        self.assertEqual(result.text('verdict'), 'PASS')

    def test_missing_coverage_rejects_without_raw_capture(self) -> None:
        # Given: a sanitized result missing the entire coverage object.
        result = load(self.root / 'perf.json')
        result.values.pop('measurement_coverage')
        # When / Then: task33 cannot accept the remaining PASS labels.
        with self.assertRaises(EvidenceError):
            performance(result, 1800)

    def test_complete_windows_accept_informational_decode_over_33ms(self) -> None:
        # Given: real-reporter results from complete synthetic 30m/2h captures.
        for filename, seconds in (('perf.json', 1800), ('soak.json', 7200)):
            with self.subTest(seconds=seconds):
                result = load(self.root / filename)
                self.assertGreater(result.child('metrics').child('decode_p95_ms').number('value'), 33)
                self.assertNotIn('capture', result.values)
                # When: task33 independently inspects the sanitized result.
                performance(result, seconds)
                # Then: the evidence satisfies only the synthetic contract.
                self.assertEqual(result.text('qualification_status'), 'COMPLETE_LIVE')

    def test_rehashed_missing_or_contradictory_coverage_rejects(self) -> None:
        # Given: matching hashes and PASS labels cannot repair summary mutations.
        mutations: tuple[tuple[tuple[str, ...], JSON], ...] = (
            (('measurement_coverage',), None),
            (('schema',), 'aa-projection-perf/1'),
            (('measurement_coverage', 'complete'), True),
            (('measurement_coverage', 'window'), None),
            (('measurement_coverage', 'window', 'end_ns'), 1_800_999_999_998),
            (('measurement_coverage', 'sampling', 'count'), True),
            (('measurement_coverage', 'sampling', 'count'), 1),
            (('measurement_coverage', 'sampling', 'first_ns'), 0),
            (('measurement_coverage', 'sampling', 'max_gap_ns'), 10_000_000_001),
            (('measurement_coverage', 'rss', 'expected'), 360),
            (('measurement_coverage', 'rss', 'valid'), 362),
            (('measurement_coverage', 'rss', 'missing'), 1),
            (('measurement_coverage', 'rss', 'max_gap_ns'), 0),
            (('measurement_coverage', 'thermal', 'valid'), 0),
            (('measurement_coverage', 'cpu', 'expected'), 361),
            (('measurement_coverage', 'cpu', 'baseline_valid'), False),
            (('measurement_coverage', 'cpu', 'measured_interval_ns'), 1),
            (('measurement_coverage', 'cpu', 'measured_interval_ns'), 1_800_000_000_001),
            (('measurement_coverage', 'cpu', 'max_gap_ns'), 1),
            (('measurement_coverage', 'cpu', 'measured_interval_ns'), True),
            (('measurement_coverage', 'ipc_consumption'), None),
            (('measurement_coverage', 'ipc_consumption', 'count'), 0),
            (('measurement_coverage', 'ipc_consumption', 'bytes'), 1),
            (('measurement_coverage', 'ipc_matches', 'count'), 361),
            (('measurement_coverage', 'ipc_matches', 'bytes'), 1),
            (('measurement_coverage', 'ipc_matches', 'max_gap_ns'), 10_000_000_001),
            (('measurement_coverage', 'decode'), None),
            (('measurement_coverage', 'decode', 'begin_ns'), 0),
            (('measurement_coverage', 'decode', 'last_ns'), 1_801_000_000_001),
            (('measurement_coverage', 'decode', 'max_gap_ns'), 0),
            (('measurement_coverage', 'decode', 'max_gap_ns'), 10_000_000_001),
            (('measurement_coverage', 'quantiles', 'decode_retained'), 1),
            (('measurement_coverage', 'quantiles', 'ipc_retained'), True),
            (('metrics', 'decode_frames', 'value'), 1),
            (('metrics', 'decode_fps', 'value'), 30),
            (('metrics', 'ipc_throughput_mbps', 'value'), 10),
            (('metrics', 'thermal_peak_c', 'value'), 99),
            (('thermal_state', 'peak_c'), 99),
        )
        for field, filename in (('perf_30m', 'perf.json'), ('soak_2h', 'soak.json')):
            original = load(self.root / filename).values
            for keys, value in mutations:
                with self.subTest(field=field, keys=keys):
                    result = Document.parse(copy.deepcopy(original))
                    child = result
                    for key in keys[:-1]:
                        child = child.child(key)
                    child.values[keys[-1]] = value
                    write(self.root / filename, result.values)
                    rebind(self.manifest, field)
                    # When / Then: task33 rejects semantics despite rebound bytes.
                    with self.assertRaises(EvidenceError):
                        check(self.manifest)
            write(self.root / filename, original)
            rebind(self.manifest, field)

    def test_reporter_gaps_remain_rejected_after_labels_are_forged(self) -> None:
        # Given: independent missing evidence with otherwise continuous progress.
        provenance = load(self.manifest).child('provenance').values
        for missing in ('startup_ipc', 'ipc_matches', 'decode', 'rss', 'cpu', 'thermal'):
            with self.subTest(missing=missing):
                raw = Document.parse(capture(1800, provenance))
                if missing in ('rss', 'cpu', 'thermal'):
                    samples = raw.array('samples')
                    for sample in (samples if missing == 'thermal' else samples[1:]):
                        row = Document.parse(sample)
                        if missing == 'thermal':
                            row.values['thermal_zones'] = []
                        else:
                            key = 'receiver_rss_mb' if missing == 'rss' else 'receiver_cpu_ticks'
                            row.values[key] = None
                else:
                    video = raw.child('video_evidence')
                    stream = 'ipc_consumption' if missing == 'startup_ipc' else missing
                    blocks = video.array('blocks')
                    for block in (blocks[1:] if missing == 'startup_ipc' else blocks):
                        row = Document.parse(block)
                        row.values[stream] = {'count': 0, 'first_ns': None, 'last_ns': None,
                            'max_internal_gap_ns': 0, 'begin_ns': None, 'bytes': 0}
                        if missing == 'startup_ipc':
                            row.values['ipc_matches'] = copy.deepcopy(row.values[stream])
                    if missing in ('startup_ipc', 'ipc_matches'):
                        video.values['ipc_latency_ns'] = [1_000_000] if missing == 'startup_ipc' else []
                    if missing == 'decode':
                        video.values['decode_latency_ns'] = []
                result = build_result(raw.values, duration_requested=None,
                    now='2026-01-01T00:30:00Z', capture_path=None, capture_sha256=None)
                self.assertEqual(result['verdict'], 'BLOCKED')
                result.update(verdict='PASS', qualification_status='COMPLETE_LIVE',
                              blocked_on=None, named_gaps=[], invalid_telemetry=[])
                # When / Then: even forged labels cannot establish coverage.
                with self.assertRaises(EvidenceError):
                    performance(Document.parse(result), 1800)

    def test_measured_budget_breach_remains_fail_with_missing_coverage(self) -> None:
        # Given: a real measured RSS breach alongside absent thermal evidence.
        provenance = load(self.manifest).child('provenance').values
        raw = Document.parse(capture(1800, provenance))
        for sample in raw.array('samples'):
            row = Document.parse(sample)
            row.values.update(receiver_rss_mb=251.0, thermal_zones=[])
        # When: the real reporter combines definite breach and missing evidence.
        result = build_result(raw.values, duration_requested=None,
            now='2026-01-01T00:30:00Z', capture_path=None, capture_sha256=None)
        # Then: the breach is FAIL, not a missing-evidence-only BLOCKED result.
        self.assertEqual(result['verdict'], 'FAIL')


if __name__ == '__main__':
    unittest.main()
