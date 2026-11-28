# SPDX-License-Identifier: GPL-3.0-or-later
"""Wireless evidence tests. All measurements, identifiers and receipts are synthetic."""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from dataclasses import asdict
from pathlib import Path

from fixture import ROOT, WIRELESS_PRIVACY_CONTROLS, binding, write
from synthetic_capture import capture_v3
from aa_measurement_coverage import NANO_THERMAL_POLICY
from aa_perf_report import build_result
from aa_projection_collect import max_wireless_startup_timeout_seconds
from check_wireless import DURATIONS, SCHEMA, check
from common import EvidenceError, load
from results import performance


def wireless_result(name: str, provenance: dict, stamp: str, *, scoped: bool = False,
                    excursions: bool = True) -> dict:
    seconds, duration = DURATIONS[name]
    # A sparse synthetic series still exercises every signed A/V gate.
    raw = capture_v3(seconds, provenance, pair_interval_ns=500_000_000)
    usb = {'readable': True, 'phone_attached': False, 'aoa_attached': False}
    snapshot = {'wifi': {'connected': True, 'channel': 36, 'frequency_mhz': 5180}}
    raw.update(mode='wireless', ap_channel=36, ap_interface='wlan0',
               build={'build_type': 'Release', 'build_dir': 'synthetic', 'verified_on': 'target'},
               link={'adb_bound': True, 'before': snapshot, 'after': snapshot,
                     'host_usb': {'before': usb, 'during': [usb], 'after': usb}})
    if scoped:
        raw['thermal_policy'] = asdict(NANO_THERMAL_POLICY)
        raw['target_hardware_state'] = provenance['target_hardware_state']
        for sample in raw['samples']:
            sample['thermal_zones'] = [
                {'zone': f'thermal_zone{index}:{family}-thermal',
                 'temp_c': None if family.startswith('cv') else 45.0}
                for index, family in enumerate(('cpu', 'gpu', 'cv0', 'cv1', 'cv2',
                                                'soc0', 'soc1', 'soc2', 'tj'))
            ]
    if excursions:
        # Exercise the exact allowance in both runs, including a pair at the hard cap.
        series = raw['av_series']['pair_skew_ns']
        for index in range(seconds // 1800):
            series[20 + 2_000 * index] = -150_000_000
    return build_result(raw, duration_requested=duration if name == 'perf_30m' else f'{seconds}s',
                        now=stamp, capture_path=None, capture_sha256=None)


def make_package(root: Path) -> None:
    provenance = {
        'source_digest': 'a' * 64, 'build_artifact_digest': 'b' * 64,
        'configuration_digest': 'c' * 64,
        'service_definitions': [{'name': 'synthetic.service', 'digest': 'd' * 64}],
        'dependency_versions': {'synthetic-library': '1.0'},
        'target_hardware_state': {'model': 'synthetic Jetson', 'power_mode': 'synthetic'},
        'package_digest': None, 'equivalence_status': 'provisional',
    }
    def receipt(name: str) -> dict:
        path = root / f'{name}.log'
        path.write_text('SYNTHETIC TEST ONLY\nphone_id=id-0123456789abcdef\n'
                        'phone_serial=[REDACTED]\nap_ssid=[REDACTED]\nwifi_psk=[REDACTED]\n')
        return binding(path)

    stages = {}
    for day, (name, (seconds, _)) in enumerate(DURATIONS.items(), start=1):
        stamp = f'2026-01-0{day}T00:00:00Z'
        path = root / f'{name}.json'
        record = root / f'{name}-receipt.json'
        write(path, wireless_result(name, provenance, stamp))
        write(record, {'generated_at_utc': stamp, 'ap_before': None, 'ap_after': None,
                       'resource_observations': 'SYNTHETIC TEST ONLY'})
        stages[name] = {'result': binding(path), 'receipt': binding(record),
                        'ap_uptime_seconds': seconds, 'helper_rss_peak_mb': 32, 'helper_crashes': 0}
    recovered = {'confirmed_active': True, 'video_resumed': True, 'audio_resumed': True,
                 'input_resumed': True, 'fresh_pairing': False}
    write(root / 'wireless.json', {
        'schema': SCHEMA, 'status': 'COMPLETE', 'provenance': provenance,
        'target': {'model': 'synthetic Jetson', 'jetpack_version': 'synthetic',
                   'l4t_version': 'synthetic', 'kernel_version': 'synthetic'},
        'phone': {'model': 'Samsung Galaxy A26', 'android_version': 'synthetic',
                  'android_auto_version': 'synthetic'},
        'ap': {'interface': 'wlan0', 'channel': 36, 'regulatory_domain': 'US',
               'confirmed_jurisdiction': 'US', 'startup_ms': 5000, 'receipt': receipt('ap')},
        'bluez': {'version': 'synthetic', 'numeric_comparison_accepted': True,
                  'helper_bond_acknowledged': True, 'approval_persisted': True,
                  'approved_bond_reused': True, 'receipt': receipt('bluez')},
        'session': {'rfcomm_channel': 8, 'bootstrap_completed': True, 'aap_tls_authenticated': True,
                    'confirmed_active': True, 'video_frames': 600, 'audio_writes': 120,
                    'inputs_forwarded': 20, 'receipt': receipt('session')},
        'performance': stages,
        'recovery': {'receiver_restart': {'elapsed_ms': 60000, **recovered},
                     'session_reconnect': {'elapsed_ms': 15000, **recovered},
                     'receipt': receipt('recovery')},
        'limitations': [{'claim': 'Physical audio timing', 'detail': 'SYNTHETIC TEST ONLY'}],
    })


class WirelessTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        source = tempfile.TemporaryDirectory(prefix='wireless-qualification-fixture-')
        cls.addClassCleanup(source.cleanup)
        cls.source = Path(source.name)
        make_package(cls.source)

    def setUp(self) -> None:
        export = tempfile.TemporaryDirectory(prefix='wireless-qualification-test-')
        self.addCleanup(export.cleanup)
        self.root = Path(export.name)
        self.manifest = self.root / 'wireless.json'
        shutil.copytree(self.source, self.root, dirs_exist_ok=True)

    def cli(self, tool: str, *args: Path) -> subprocess.CompletedProcess[str]:
        return subprocess.run([sys.executable, '-B', str(ROOT / 'tools/qualification' / tool),
                               *map(str, args)], capture_output=True, text=True, timeout=15,
                              check=False)

    def assert_rejected(self, file: Path, field: tuple, value: object,
                        error: str = '') -> None:
        document = load(self.source / file.name).values
        parent = document
        for key in field[:-1]:
            parent = parent[key]
        parent[field[-1]] = value
        write(file, document)
        if file != self.manifest:
            manifest = load(self.source / self.manifest.name).values
            manifest['performance'][file.stem]['result'] = binding(file)
            write(self.manifest, manifest)
        try:
            with self.assertRaisesRegex(EvidenceError, error):
                check(self.manifest)
        finally:
            shutil.copyfile(self.source / file.name, file)
            if file != self.manifest:
                shutil.copyfile(self.source / self.manifest.name, self.manifest)

    def test_synthetic_package_passes_both_clis(self) -> None:
        self.assertEqual(self.cli('check_wireless.py', self.manifest).returncode, 0)
        self.assertEqual(self.cli('check_privacy.py', self.root).returncode, 0)
        self.assertEqual(self.cli('check_wireless.py').returncode, 2)

    def test_pending_template_fails_and_docs_pass_privacy(self) -> None:
        docs = ROOT / 'docs/qualification'
        check_result = self.cli('check_wireless.py', docs / 'wireless.json')
        self.assertEqual(check_result.returncode, 1)
        self.assertEqual(self.cli('check_privacy.py', docs).returncode, 0)
        with self.assertRaisesRegex(EvidenceError, '^INVALID_FIELD:status$'):
            check(docs / 'wireless.json')
        pending = load(docs / 'wireless.json').values
        write(self.manifest, {**pending, 'status': 'COMPLETE'})
        with self.assertRaises(EvidenceError):
            check(self.manifest)

    def test_manifest_gates(self) -> None:
        cases = (
            (('status',), 'OWNER-SIGNED-REDUCED-SCOPE'),
            (('target', 'kernel_version'), 'PENDING'), (('phone', 'android_auto_version'), None),
            (('phone', 'model'), 'synthetic different model'),
            (('ap', 'interface'), '../wifi'), (('ap', 'channel'), 44),
            (('ap', 'regulatory_domain'), '00'), (('ap', 'confirmed_jurisdiction'), 'CA'),
            (('ap', 'startup_ms'), 5000.001), (('ap', 'startup_ms'), -0.001),
            (('bluez', 'version'), 'PENDING'),
            (('bluez', 'numeric_comparison_accepted'), False),
            (('bluez', 'helper_bond_acknowledged'), False),
            (('bluez', 'approval_persisted'), False), (('bluez', 'approved_bond_reused'), False),
            (('session', 'rfcomm_channel'), 9), (('session', 'bootstrap_completed'), False),
            (('session', 'aap_tls_authenticated'), False), (('session', 'confirmed_active'), False),
            (('session', 'video_frames'), 0), (('session', 'audio_writes'), -1),
            (('session', 'inputs_forwarded'), True),
            (('recovery', 'receiver_restart', 'elapsed_ms'), 60000.001),
            (('recovery', 'session_reconnect', 'elapsed_ms'), 15000.001),
            (('recovery', 'session_reconnect', 'elapsed_ms'), -0.001),
            (('recovery', 'session_reconnect', 'confirmed_active'), False),
            (('recovery', 'session_reconnect', 'video_resumed'), False),
            (('recovery', 'receiver_restart', 'audio_resumed'), False),
            (('recovery', 'receiver_restart', 'input_resumed'), False),
            (('recovery', 'receiver_restart', 'fresh_pairing'), True),
            (('limitations',), []), (('limitations', 0, 'detail'), 'PENDING'),
            (('provenance', 'source_digest'), '0'), (('provenance', 'build_artifact_digest'), '0'),
            (('provenance', 'configuration_digest'), '0'),
        )
        for field, value in cases:
            with self.subTest(field=field, value=value):
                self.assert_rejected(self.manifest, field, value)

    def test_each_stage_enforces_auxiliary_measurements(self) -> None:
        for name, (seconds, _) in DURATIONS.items():
            for field, value in (('ap_uptime_seconds', seconds - 0.001),
                                 ('helper_rss_peak_mb', 32.001), ('helper_rss_peak_mb', None),
                                 ('helper_rss_peak_mb', 0), ('helper_crashes', 1)):
                with self.subTest(stage=name, field=field, value=value):
                    self.assert_rejected(self.manifest, ('performance', name, field), value)

    def test_each_rehashed_native_result_enforces_gates(self) -> None:
        for name, (seconds, _) in DURATIONS.items():
            file = self.root / f'{name}.json'
            cases = (
                (('ap_channel',), 44), (('ap_interface',), 'wlan1'), (('mode',), 'wired'),
                (('link', 'adb_bound'), False), (('link', 'host_usb', 'during'), []),
                (('link', 'host_usb', 'after', 'aoa_attached'), True),
                (('duration_requested',), '10s'), (('duration_seconds',), seconds - 1),
                (('run_status', 'active_projection_seconds'), seconds - 1),
                (('run_status', 'display_exit_code'), 1),
                (('measurement_origin',), 'offline'), (('build', 'build_type'), 'Debug'),
                (('build', 'verified_on'), 'none'), (('build', 'allow_unoptimized_build'), True),
                (('provenance', 'configuration_digest'), 'e' * 64), (('crashes',), 1),
                (('metrics', 'receiver_rss_peak_mb', 'value'), 250.001),
                (('metrics', 'receiver_cpu_sustained_pct_one_core', 'value'), 50.001),
                (('metrics', 'rss_growth_pct', 'value'), 10),
                (('metrics', 'audio_latency_ms', 'value'), 200.001),
                (('metrics', 'av_p99_abs_ms', 'value'), 50.001),
                (('metrics', 'av_window_mean_abs_ms', 'value'), 40.001),
                (('metrics', 'av_coverage_pct', 'value'), 89.999),
                (('metrics', 'av_excursions', 'value'), seconds // 1800 + 1),
                (('metrics', 'av_hard_cap_pairs', 'value'), 1),
                (('av_contract', 'excursion_pairs', 0, 'skew_ms'), -150.001),
                (('av_contract', 'excursion_pairs', 0, 'offset_s'), seconds),
                (('av_contract', 'excursion_pairs', 0, 'skew_ms'), 90),
                (('av_contract', 'excursion_pairs_truncated'), True),
                (('thermal_state', 'zones'), []), (('interrupted',), True),
            )
            for field, value in cases:
                with self.subTest(stage=name, field=field, value=value):
                    self.assert_rejected(file, field, value)

    def install_stage_file(self, stage: str, kind: str, path: Path, value: dict) -> None:
        write(path, value)
        manifest = load(self.manifest)
        manifest.child('performance').child(stage).values[kind] = binding(path)
        write(self.manifest, manifest.values)

    def test_missing_ap_regulatory_evidence_fails(self) -> None:
        for section, field, code in (
            (None, 'ap', 'UNEXPECTED_OR_MISSING_FIELDS'),
            ('ap', 'regulatory_domain', 'UNEXPECTED_OR_MISSING_FIELDS'),
            ('ap', 'confirmed_jurisdiction', 'UNEXPECTED_OR_MISSING_FIELDS'),
            ('ap', 'receipt', 'UNEXPECTED_OR_MISSING_FIELDS'),
        ):
            with self.subTest(field=field):
                doc = load(self.source / 'wireless.json')
                (doc if section is None else doc.child(section)).values.pop(field)
                write(self.manifest, doc.values)
                with self.assertRaisesRegex(EvidenceError, code):
                    check(self.manifest)
                self.assertEqual(self.cli('check_wireless.py', self.manifest).returncode, 1)

    def test_phone_observations_must_match_channel_and_frequency(self) -> None:
        for stage in DURATIONS:
            for moment in ('before', 'after'):
                for field, value in (('channel', 44), ('frequency_mhz', 5220)):
                    with self.subTest(stage=stage, moment=moment, field=field):
                        self.assert_rejected(self.root / f'{stage}.json',
                                             ('link', moment, 'wifi', field), value,
                                             f'^PHONE_AP_OBSERVATION:{moment}:{field}$')

    def test_missing_phone_observations_require_run_receipt(self) -> None:
        stage = 'perf_30m'
        path = self.root / f'{stage}.json'
        receipt_path = self.root / f'{stage}-receipt.json'
        result = load(path)
        result.child('link').values['before'] = None
        result.child('link').child('after').child('wifi').values['frequency_mhz'] = None
        self.install_stage_file(stage, 'result', path, result.values)
        with self.assertRaisesRegex(EvidenceError, '^AP_CORROBORATION_MISSING:before$'):
            check(self.manifest)

        receipt = load(receipt_path).values
        evidence = {'channel': 36, 'frequency_mhz': 5180, 'source': 'target-radio',
                    'evidence': 'Synthetic active AP radio observation for this stage'}
        receipt.update(ap_before=evidence, ap_after=evidence.copy())
        self.install_stage_file(stage, 'receipt', receipt_path, receipt)
        check(self.manifest)

        for field, value, code in (
            ('source', 'operator-config', 'AP_CORROBORATION_SOURCE'),
            ('channel', 44, 'INVALID_FIELD:channel'),
            ('frequency_mhz', 5220, 'INVALID_FIELD:frequency_mhz'),
            ('evidence', 'PENDING', 'PLACEHOLDER:evidence'),
        ):
            with self.subTest(field=field):
                receipt['ap_after'] = {**evidence, field: value}
                self.install_stage_file(stage, 'receipt', receipt_path, receipt)
                with self.assertRaisesRegex(EvidenceError, code):
                    check(self.manifest)
        receipt['ap_after'] = evidence
        receipt['generated_at_utc'] = '2026-01-03T00:00:00Z'
        self.install_stage_file(stage, 'receipt', receipt_path, receipt)
        with self.assertRaisesRegex(EvidenceError, 'INVALID_FIELD:generated_at_utc'):
            check(self.manifest)

    def test_wireless_startup_has_bounded_grace(self) -> None:
        for stage, (seconds, _) in DURATIONS.items():
            path = self.root / f'{stage}.json'
            with self.subTest(stage=stage):
                self.assert_rejected(path, ('run_status', 'startup_wait_seconds'), -0.001)
                max_wait = max_wireless_startup_timeout_seconds(seconds)
                self.assert_rejected(path, ('run_status', 'startup_wait_seconds'), max_wait + 0.001,
                                     '^PERF_STARTUP_GRACE$')
                self.assert_rejected(path, ('startup_basis',), 'receiver',
                                     '^INVALID_FIELD:startup_basis$')
        for stage in DURATIONS:
            path = self.root / f'{stage}.json'
            result = load(path)
            result.child('run_status').values['startup_wait_seconds'] = 90
            self.install_stage_file(stage, 'result', path, result.values)
        check(self.manifest)

    def test_wireless_runs_require_matching_thermal_policy(self) -> None:
        manifest = load(self.manifest)
        model = NANO_THERMAL_POLICY.model
        manifest.child('target').values['model'] = model
        provenance = manifest.child('provenance')
        provenance.child('target_hardware_state').values['model'] = model
        write(self.manifest, manifest.values)
        for stage, (seconds, _) in DURATIONS.items():
            path = self.root / f'{stage}.json'
            stamp = load(path).text('generated_at_utc')
            result = wireless_result(stage, provenance.values, stamp, scoped=stage == 'perf_30m')
            self.install_stage_file(stage, 'result', path, result)
            performance(load(path), seconds, mode='wireless')
        with self.assertRaisesRegex(EvidenceError, '^THERMAL_POLICY_RUN_MISMATCH$'):
            check(self.manifest)

        path = self.root / 'soak_2h.json'
        scoped_soak = wireless_result('soak_2h', provenance.values,
                                      load(path).text('generated_at_utc'), scoped=True)
        self.install_stage_file('soak_2h', 'result', path, scoped_soak)
        check(self.manifest)

    def test_excursion_summary_consistency(self) -> None:
        short = self.root / 'perf_30m.json'
        long = self.root / 'soak_2h.json'
        self.assert_rejected(short, ('av_contract', 'excursion_pairs', 0, 'skew_ms'), -100,
                             '^AV_EXCURSION_MAX$')
        self.assert_rejected(long, ('av_contract', 'excursion_pairs', 1, 'offset_s'), 0,
                             '^AV_EXCURSION_TIMING$')

        manifest = load(self.manifest)
        result = wireless_result('perf_30m', manifest.child('provenance').values,
                                 load(short).text('generated_at_utc'), excursions=False)
        self.install_stage_file('perf_30m', 'result', short, result)
        check(self.manifest)
        result['av_contract']['abs_max_ms'] = 100.0
        result['metrics']['av_max_abs_ms']['value'] = 100.0
        self.install_stage_file('perf_30m', 'result', short, result)
        with self.assertRaisesRegex(EvidenceError, '^AV_EXCURSION_MISSING$'):
            check(self.manifest)

    def test_retired_result_filenames_fail(self) -> None:
        result = load(self.root / 'perf_30m.json').values
        for tag in ('interrupted', 'disqualified'):
            with self.subTest(tag=tag):
                path = self.root / f'perf_30m.{tag}.json'
                self.install_stage_file('perf_30m', 'result', path, result)
                with self.assertRaisesRegex(EvidenceError, '^RETIRED_RESULT$'):
                    check(self.manifest)

    def test_distinct_results_and_bound_receipts(self) -> None:
        stages = load(self.manifest).child('performance')
        self.assert_rejected(self.manifest, ('performance', 'soak_2h', 'result'),
                             stages.child('perf_30m').get('result'))
        ap_receipt = load(self.manifest).child('ap').get('receipt')
        self.assert_rejected(self.manifest, ('bluez', 'receipt'), ap_receipt,
                             '^RECEIPTS_MUST_BE_SEPARATE$')
        self.assert_rejected(self.manifest, ('ap', 'receipt', 'path'), '../ap.log')
        self.assert_rejected(self.manifest, ('ap', 'receipt', 'sha256'), 'f' * 64)
        (self.root / 'session.log').unlink()
        self.assertEqual(self.cli('check_wireless.py', self.manifest).returncode, 1)

    def test_distinct_run_timestamps_even_when_both_receipts_match(self) -> None:
        stamp = load(self.root / 'perf_30m.json').get('generated_at_utc')
        for kind, filename in (('result', 'soak_2h.json'), ('receipt', 'soak_2h-receipt.json')):
            path = self.root / filename
            document = load(path).values
            document['generated_at_utc'] = stamp
            self.install_stage_file('soak_2h', kind, path, document)
        with self.assertRaisesRegex(EvidenceError, '^PERF_RUNS_MUST_BE_DISTINCT$'):
            check(self.manifest)

    def test_privacy_rejects_unreferenced_content_with_no_echo(self) -> None:
        controls = (
            'phone_identifier=TESTONLY_RAW', 'phoneSerial=TESTONLY_RAW',
            'adb_serial=TESTONLY_RAW', 'ap_ssid=TESTONLY_RAW', 'wifiPsk=TESTONLY_RAW',
            'wifi_passphrase=TESTONLY_RAW', 'BluetoothMAC=021122334455',
            '02:11:22:33:44:55', r'{"ap\u005fssid":"TESTONLY_RAW"}',
            r'{"peer_mac":"02\u003a11\u003a22\u003a33\u003a44\u003a55"}',
        )
        for control in controls:
            with self.subTest(control=control):
                path = self.root / ('privacy.jsonl' if control.startswith('{') else 'privacy.log')
                path.write_text(control + '\n')
                for tool, target in (('check_wireless.py', self.manifest), ('check_privacy.py', self.root)):
                    result = self.cli(tool, target)
                    output = result.stdout + result.stderr
                    self.assertEqual(result.returncode, 1, output)
                    for private in ('TESTONLY_RAW', '021122334455', '02:11:22:33:44:55'):
                        self.assertNotIn(private, output)
                path.unlink()

    def test_privacy_rejects_wireless_stack_output_without_echo(self) -> None:
        path = self.root / 'private-input.log'
        for index, (control, marker) in enumerate(WIRELESS_PRIVACY_CONTROLS):
            with self.subTest(format=index):
                path.write_text(control + '\n', encoding='utf-8')
                result = self.cli('check_wireless.py', self.manifest)
                self.assertEqual(result.returncode, 1)
                self.assertNotIn(marker, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
