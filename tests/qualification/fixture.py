# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic evidence only: never a live qualification record."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/qualification'))
sys.path.insert(0, str(ROOT / 'tools/perf'))
from common import JSON, Document
from aa_perf_report import build_result
from synthetic_capture import capture


class PackageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix='wired-qualification-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.manifest = make_package(self.root)

    def cli(self, tool: str) -> subprocess.CompletedProcess[str]:
        target = self.manifest if tool == 'check_wired.py' else self.root
        return subprocess.run([sys.executable, '-B', str(ROOT / 'tools/qualification' / tool),
                               str(target)], capture_output=True, text=True, timeout=15,
                              check=False)


def write(path: Path, value: JSON) -> None:
    path.write_text(json.dumps(value, allow_nan=False), encoding='utf-8')


def binding(path: Path) -> dict[str, JSON]:
    return {'path': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def make_package(root: Path) -> Path:
    provenance: dict[str, JSON] = {
        'source_digest': 'a' * 64, 'build_artifact_digest': 'b' * 64,
        'configuration_digest': 'c' * 64,
        'service_definitions': [{'name': 'synthetic.service', 'digest': 'd' * 64}],
        'dependency_versions': {'synthetic-library': '1.0'},
        'target_hardware_state': {'model': 'synthetic Jetson', 'power_mode': 'synthetic'},
        'package_digest': None, 'equivalence_status': 'provisional',
    }
    projection: dict[str, JSON] = {
        'task': '30. Qualify wired end-to-end projection', 'phase': 'B (live device)',
        'host': 'sha256:' + 'a' * 16, 'port': '1-2.1', 'status': 'PASS', 'blocked_on': '',
        'observables': {'projection_reached': True, 'video_present': True,
                        'audio_present': True, 'input_evidence': True, 'startup_budget_ms': 900},
        'note': 'SYNTHETIC TEST ONLY',
    }
    scenarios: list[JSON] = []
    evidence = {
        'usb_unplug_replug': 'dip+frames+pump+input',
        'receiver_restart': 'process_dip+active+frames+pump+input',
        'consumer_restart': 'process_dip+frames+pump+input',
        'phone_lock_unlock': 'dip+frames+pump+input',
        'phone_aa_restart': 'dip+frames+pump+input',
        'unknown_phone': 'attributed_rejection',
    }
    for name, observed in evidence.items():
        scenarios.append({'name': name, 'status': 'PASS', 'recovery_ms': 100,
                          'elapsed_ms': 100, 'retries': 0, 'frames_painted_baseline': 2,
                          'evidence': observed, 'detail': 'SYNTHETIC TEST ONLY'})
    scenarios.append({'name': 'malformed_input', 'status': 'N/A',
                      'detail': 'host_covered_by_ctest_wired_recovery_no_shell_injector'})
    recovery: dict[str, JSON] = {
        'task': '31. Implement wired reconnect and recovery matrix', 'phase': 'B (live device)',
        'port': '1-2.1', 'status': 'PASS', 'blocked_on': '', 'budget_ms': 5000,
        'only': None, 'subset': False, 'scenarios': scenarios,
    }
    write(root / 'projection.json', projection)
    write(root / 'recovery.json', recovery)
    for name, seconds, stamp in (('perf.json', 1800, '2026-01-01T00:30:00Z'),
                                 ('soak.json', 7200, '2026-01-01T02:30:00Z')):
        result = build_result(capture(seconds, provenance), duration_requested=None, now=stamp,
                               capture_path=None, capture_sha256=None)
        result['notes'].append('SYNTHETIC TEST ONLY; not observed on hardware')
        write(root / name, result)
    (root / 'session.jsonl').write_text('{"event":"active","identity":"sha256:'
                                        + 'a' * 64 + '"}\n', encoding='utf-8')
    (root / 'sanitized.log').write_text('SYNTHETIC TEST ONLY: media delivered\n', encoding='utf-8')
    manifest: dict[str, JSON] = {
        'schema': 'aa-wired-qualification/1',
        'target': {'model': 'synthetic Jetson', 'jetpack_version': '6.synthetic',
                   'l4t_version': '36.synthetic', 'kernel_version': '5.synthetic'},
        'phone': {'model': 'synthetic phone', 'android_version': 'synthetic-15',
                  'android_auto_version': 'synthetic-13'},
        'usb_topology': {'controller': 'synthetic xhci', 'port': '1-2.1',
                         'hub': 'synthetic hub', 'cable': 'synthetic USB cable'},
        'limitations': ['SYNTHETIC ONLY; completion endpoints are not photons/speaker output',
                        'decode p95 budget and software fallback claims withdrawn'],
        'provenance': provenance,
    }
    for field, filename in (('task30', 'projection.json'), ('task31', 'recovery.json'),
                            ('perf_30m', 'perf.json'), ('soak_2h', 'soak.json'),
                            ('session_trace', 'session.jsonl'), ('redacted_log', 'sanitized.log')):
        manifest[field] = binding(root / filename)
    write(root / 'wired.json', manifest)
    return root / 'wired.json'


def rebind(manifest: Path, field: str) -> None:
    document = Document.parse(json.loads(manifest.read_text()))
    path = manifest.parent / document.child(field).text('path')
    write(manifest, {**document.values, field: binding(path)})
