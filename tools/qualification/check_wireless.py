# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Validate a redacted wireless qualification package for full compliance."""
from __future__ import annotations

import argparse
import re
from pathlib import Path

from check_privacy import scan
from common import Document, EvidenceError, artifact, load, package_file, require
from results import aa_av_contract, load_performance, performance, provenance
from aa_transport import link_gap, valid_interface

SCHEMA = 'aa-wireless-qualification/1'
DURATIONS = {'perf_30m': (1800, '30m'), 'soak_2h': (7200, '2h')}


def checked_section(doc: Document, name: str, fields: set[str]) -> Document:
    section = doc.child(name)
    section.keys(fields)
    return section


def access_point(doc: Document) -> Document:
    ap = checked_section(doc, 'ap', {'interface', 'channel', 'regulatory_domain',
                                    'confirmed_jurisdiction', 'startup_ms', 'receipt'})
    require(valid_interface(ap.get('interface')), 'AP_INTERFACE')
    ap.equal('channel', 36)
    country = ap.text('confirmed_jurisdiction')
    require(re.fullmatch('[A-Z]{2}', country) is not None, 'AP_JURISDICTION')
    ap.equal('regulatory_domain', country)
    require(0 <= ap.number('startup_ms') <= 5000, 'AP_STARTUP')
    return ap


def session(doc: Document) -> Document:
    section = checked_section(doc, 'session', {'rfcomm_channel', 'bootstrap_completed',
                                               'aap_tls_authenticated', 'confirmed_active',
                                               'video_frames', 'audio_writes', 'inputs_forwarded',
                                               'receipt'})
    section.equal('rfcomm_channel', 8)
    for field in ('bootstrap_completed', 'aap_tls_authenticated', 'confirmed_active'):
        section.equal(field, True)
    for field in ('video_frames', 'audio_writes', 'inputs_forwarded'):
        require(section.integer(field) > 0, 'MISSING_SESSION_PROGRESS')
    return section


def recovery(doc: Document) -> Document:
    section = checked_section(doc, 'recovery', {'receiver_restart', 'session_reconnect', 'receipt'})
    for name, deadline in (('receiver_restart', 60000), ('session_reconnect', 15000)):
        attempt = checked_section(section, name, {'elapsed_ms', 'confirmed_active', 'video_resumed',
                                                  'audio_resumed', 'input_resumed', 'fresh_pairing'})
        require(0 <= attempt.number('elapsed_ms') <= deadline, 'RECOVERY_DEADLINE:' + name)
        for field in ('confirmed_active', 'video_resumed', 'audio_resumed', 'input_resumed'):
            attempt.equal(field, True)
        attempt.equal('fresh_pairing', False)
    return section


def excursion_pairs(result: Document, active: float) -> None:
    """Check every exported over-90 ms pair against the native soak summary."""
    contract = result.child('av_contract')
    pairs = contract.array('excursion_pairs')
    contract.equal('excursion_pairs_truncated', False)
    contract.equal('excursions', len(pairs))
    require(len(pairs) <= contract.integer('pairs'), 'AV_EXCURSION_COUNT')
    maximum = contract.number('abs_max_ms')
    require(0 <= contract.number('abs_p99_ms') <= maximum, 'AV_MAX_P99')
    magnitudes: list[float] = []
    previous = -1.0
    for raw in pairs:
        pair = Document.parse(raw)
        offset = pair.number('offset_s')
        require(0 <= offset < active and offset >= previous, 'AV_EXCURSION_TIMING')
        previous = offset
        magnitude = abs(pair.number('skew_ms'))
        require(aa_av_contract.AV_SOAK_EXCURSION_MS < magnitude <=
                aa_av_contract.AV_SOAK_HARD_CAP_MS, 'AV_EXCURSION_MAGNITUDE')
        magnitudes.append(magnitude)
    if magnitudes:
        require(maximum == max(magnitudes), 'AV_EXCURSION_MAX')
    else:
        require(maximum <= aa_av_contract.AV_SOAK_EXCURSION_MS, 'AV_EXCURSION_MISSING')


def check_run(result: Document, stage: Document, ap: Document,
              evidence_provenance: Document, seconds: int, duration: str) -> None:
    performance(result, seconds, mode='wireless')
    require(result.get('duration_requested') in (duration, f'{seconds}s'),
            'PERF_REQUESTED_DURATION')
    result.equal('startup_basis', 'harness_launch_to_active')
    result.equal('ap_channel', ap.integer('channel'))
    result.equal('ap_interface', ap.text('interface'))
    require(link_gap(result.get('link')) is None, 'WIRELESS_LINK')
    result.equal('provenance', evidence_provenance.values)
    build = result.child('build')
    build.equal('build_type', 'Release')
    require(build.get('verified_on') in ('target', 'host'), 'UNVERIFIED_RELEASE')
    build.equal('allow_unoptimized_build', False)
    build.equal('non_qualifying', None)
    active = result.child('run_status').number('active_projection_seconds')
    require(stage.number('ap_uptime_seconds') >= active, 'AP_UPTIME')
    # The current native reporter always emits a null helper RSS metric. This
    # auxiliary measurement covers the same active window as the native result.
    require(0 < stage.number('helper_rss_peak_mb') <= 32, 'HELPER_RSS')
    stage.equal('helper_crashes', 0)
    excursion_pairs(result, active)


def check(manifest: Path) -> None:
    root = manifest.absolute().parent
    scan(root)
    doc = load(package_file(root, manifest.name))
    doc.keys({'schema', 'status', 'target', 'ap', 'bluez', 'phone', 'session', 'performance',
              'recovery', 'limitations', 'provenance'})
    doc.equal('schema', SCHEMA)
    doc.equal('status', 'COMPLETE')
    for name, fields in (
        ('target', {'model', 'jetpack_version', 'l4t_version', 'kernel_version'}),
        ('phone', {'model', 'android_version', 'android_auto_version'}),
    ):
        section = checked_section(doc, name, fields)
        for field in fields:
            section.text(field)
    doc.child('phone').equal('model', 'Samsung Galaxy A26')
    ap = access_point(doc)
    bluez = checked_section(doc, 'bluez', {'version', 'numeric_comparison_accepted',
                                          'helper_bond_acknowledged', 'approval_persisted',
                                          'approved_bond_reused', 'receipt'})
    bluez.text('version')
    for field in ('numeric_comparison_accepted', 'helper_bond_acknowledged',
                  'approval_persisted', 'approved_bond_reused'):
        bluez.equal(field, True)
    limitations = doc.array('limitations')
    require(bool(limitations), 'MISSING_LIMITATIONS')
    for raw in limitations:
        row = Document.parse(raw)
        row.keys({'claim', 'detail'})
        row.text('claim')
        row.text('detail')
    evidence_provenance = doc.child('provenance')
    provenance(evidence_provenance)
    evidence_provenance.child('target_hardware_state').equal('model',
                                                             doc.child('target').text('model'))

    sections = (ap, bluez, session(doc), recovery(doc))
    receipts = {artifact(root, section.child('receipt')) for section in sections}
    require(len(receipts) == len(sections), 'RECEIPTS_MUST_BE_SEPARATE')

    stages = checked_section(doc, 'performance', set(DURATIONS))
    timestamps: set[str] = set()
    for name, (seconds, duration) in DURATIONS.items():
        stage = checked_section(stages, name, {'result', 'receipt', 'ap_uptime_seconds',
                                               'helper_rss_peak_mb', 'helper_crashes'})
        result_path = artifact(root, stage.child('result'))
        receipt_path = artifact(root, stage.child('receipt'))
        require(result_path != receipt_path and not receipts & {result_path, receipt_path},
                'RECEIPTS_MUST_BE_SEPARATE')
        receipts.update((result_path, receipt_path))
        result = load_performance(result_path)
        check_run(result, stage, ap, evidence_provenance, seconds, duration)
        timestamps.add(result.text('generated_at_utc'))
    require(len(timestamps) == len(DURATIONS), 'PERF_RUNS_MUST_BE_DISTINCT')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    try:
        check(args.manifest)
    except (EvidenceError, OSError, UnicodeError, ValueError, RecursionError):
        print('wireless-check: FAIL incomplete, invalid or unsafe package (content withheld)')
        return 1
    print('wireless-check: PASS full evidence contract')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
