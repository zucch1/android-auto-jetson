# SPDX-License-Identifier: GPL-3.0-or-later
"""Semantic checks of native task30/31/32 results, not their PASS labels alone."""
from __future__ import annotations

import math
import sys
from pathlib import Path
from typing import Final

from common import DIGEST, Document, EvidenceError, require

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'perf'))
import aa_perf_schema
from aa_measurement_coverage import CoverageError, coverage_gaps, parse_coverage

RECOVERY: Final = {
    'usb_unplug_replug': 'dip+frames+pump+input',
    'receiver_restart': 'process_dip+active+frames+pump+input',
    'consumer_restart': 'process_dip+frames+pump+input',
    'phone_lock_unlock': 'dip+frames+pump+input',
    'phone_aa_restart': 'dip+frames+pump+input',
    'unknown_phone': 'attributed_rejection',
}
GATES: Final = {
    'receiver_rss_peak_mb': (250, 'MB'),
    'receiver_cpu_sustained_pct_one_core': (50, '% of one core'),
    'rss_growth_pct': (10, '%'),
    'av_sync_ms': (50, 'ms'),
    'audio_latency_ms': (200, 'ms'),
}
RECORDED: Final = ('receiver_rss_mean_mb', 'receiver_cpu_peak_pct_one_core',
                   'frame_drops', 'decode_p50_ms', 'decode_p95_ms', 'decode_fps',
                   'decode_frames', 'ipc_throughput_mbps', 'ipc_p95_added_latency_ms',
                   'thermal_peak_c')


def provenance(doc: Document) -> None:
    doc.equal('package_digest', None)
    doc.equal('equivalence_status', 'provisional')
    for name in ('source_digest', 'build_artifact_digest', 'configuration_digest'):
        require(DIGEST.fullmatch(doc.text(name)) is not None, 'INVALID_PROVENANCE_DIGEST')
    services = doc.array('service_definitions')
    require(bool(services), 'MISSING_SERVICES')
    for raw in services:
        service = Document.parse(raw)
        service.text('name')
        require(DIGEST.fullmatch(service.text('digest')) is not None, 'INVALID_SERVICE_DIGEST')
    dependencies = doc.child('dependency_versions')
    require(bool(dependencies.values), 'MISSING_DEPENDENCIES')
    for name in dependencies.values:
        require(bool(name.strip()), 'EMPTY_DEPENDENCY_NAME')
        dependencies.text(name)
    require(bool(doc.child('target_hardware_state').values), 'MISSING_HARDWARE_STATE')


def projection(doc: Document) -> None:
    doc.equal('task', '30. Qualify wired end-to-end projection')
    doc.equal('phase', 'B (live device)')
    doc.equal('status', 'PASS')
    doc.equal('blocked_on', '')
    obs = doc.child('observables')
    for name in ('projection_reached', 'video_present', 'audio_present', 'input_evidence'):
        obs.equal(name, True)
    require(0 <= obs.number('startup_budget_ms') <= 15000, 'PROJECTION_STARTUP_BUDGET')


def recovery(doc: Document) -> None:
    doc.equal('task', '31. Implement wired reconnect and recovery matrix')
    doc.equal('phase', 'B (live device)')
    doc.equal('status', 'PASS')
    doc.equal('blocked_on', '')
    doc.equal('subset', False)
    doc.equal('only', None)
    budget = doc.integer('budget_ms')
    require(0 < budget <= 5000, 'RECOVERY_BUDGET')
    seen: set[str] = set()
    for raw in doc.array('scenarios'):
        row = Document.parse(raw)
        name = row.text('name')
        require(name not in seen, 'DUPLICATE_SCENARIO')
        seen.add(name)
        if name == 'malformed_input':
            row.equal('status', 'N/A')
            row.equal('detail', 'host_covered_by_ctest_wired_recovery_no_shell_injector')
            continue
        require(name in RECOVERY, 'UNKNOWN_SCENARIO')
        row.equal('status', 'PASS')
        row.equal('evidence', RECOVERY[name])
        require(0 <= row.number('recovery_ms') < budget, 'RECOVERY_DEADLINE')
        require(row.number('elapsed_ms') >= row.number('recovery_ms'), 'RECOVERY_ELAPSED')
        if name != 'unknown_phone':
            require(row.integer('retries') >= 0, 'RECOVERY_RETRIES')
            require(row.integer('frames_painted_baseline') >= 0, 'RECOVERY_FRAME_BASELINE')
    require(seen == set(RECOVERY) | {'malformed_input'}, 'INCOMPLETE_RECOVERY_MATRIX')


def performance(doc: Document, seconds: int) -> None:
    doc.equal('task', '32. Measure wired performance and two-hour soak')
    require('capture' not in doc.values, 'RAW_CAPTURE_REFERENCE')
    doc.equal('schema', 'aa-projection-perf/2')
    schema = aa_perf_schema.load_schema(aa_perf_schema.DEFAULT_SCHEMA)
    require(not aa_perf_schema.validate(doc.values, schema), 'PERF_SCHEMA')
    doc.equal('measurement_origin', 'live')
    doc.equal('qualification_status', 'COMPLETE_LIVE')
    doc.equal('verdict', 'PASS')
    doc.equal('blocked_on', None)
    for name in ('named_gaps', 'provenance_gaps', 'invalid_telemetry', 'violations'):
        doc.equal(name, [])
    doc.equal('crashes', 0)
    require(doc.number('duration_seconds') >= seconds, 'INCOMPLETE_PERF_DURATION')
    run = doc.child('run_status')
    for name in ('display_survived', 'display_connected', 'receiver_stopped_by_collector'):
        run.equal(name, True)
    run.equal('early_exit', False)
    run.equal('receiver_exit_code', 0)
    run.equal('projection_failure', None)
    run.equal('terminal_errors', [])
    active = run.number('active_projection_seconds')
    require(seconds <= active <= doc.number('duration_seconds'), 'INCOMPLETE_ACTIVE_WINDOW')
    try:
        coverage = parse_coverage(doc.get('measurement_coverage'))
        gaps = coverage_gaps(coverage, active)
    except CoverageError as error:
        raise EvidenceError('INVALID_PERF_COVERAGE') from error
    require(not gaps, 'INCOMPLETE_PERF_COVERAGE')
    require(0 <= run.number('startup_wait_seconds') <= 60, 'PERF_STARTUP_GRACE')
    require(run.integer('startup_ms') >= 0, 'PERF_STARTUP')
    for name in ('video_units', 'audio_chunks'):
        require(run.integer(name) > 0, 'MISSING_MEDIA_PROGRESS')
    metrics = doc.child('metrics')
    for name, (ceiling, unit) in GATES.items():
        metric = metrics.child(name)
        value = metric.number('value')
        require(value < ceiling if name == 'rss_growth_pct' else 0 <= value <= ceiling,
                'PERF_BUDGET:' + name)
        require(metric.number('budget_max') == ceiling, 'PERF_METRIC_BUDGET')
        metric.equal('gated', True)
        metric.equal('status', 'PASS')
        metric.equal('unit', unit)
        if name == 'rss_growth_pct':
            metric.equal('op', 'lt')
        else:
            require(metric.values.get('op', 'le') == 'le', 'PERF_METRIC_OPERATOR')
    helper = metrics.child('helper_rss_mb')
    helper.equal('value', None)
    helper.equal('status', 'NOT_REQUIRED')
    helper.equal('gated', False)
    for name in RECORDED:
        metrics.child(name).number('value')
    thermal = doc.child('thermal_state')
    peak = thermal.number('peak_c')
    require(bool(thermal.array('zones')), 'MISSING_THERMAL_ZONES')
    require(metrics.child('thermal_peak_c').number('value') == peak,
            'PERF_THERMAL_PEAK')
    zone_peaks = [Document.parse(zone).number('temp_c') for zone in thermal.array('zones')]
    require(max(zone_peaks) == peak, 'PERF_THERMAL_ZONES')
    require(metrics.child('decode_frames').number('value') == coverage.decode.count,
            'PERF_DECODE_COUNT')
    for name, stream, scale in (('decode_fps', coverage.decode, 1e9),
                                ('ipc_throughput_mbps', coverage.ipc_consumption, 8000)):
        if stream.begin_ns is None or stream.last_ns is None:
            raise EvidenceError('PERF_RATE_BOUNDS')
        duration = stream.last_ns - stream.begin_ns
        require(duration > 0, 'PERF_RATE_DURATION')
        numerator = stream.count if name == 'decode_fps' else stream.bytes
        require(math.isclose(metrics.child(name).number('value'), numerator * scale / duration,
                             rel_tol=1e-12, abs_tol=1e-12), 'PERF_RATE:' + name)
    provenance(doc.child('provenance'))
