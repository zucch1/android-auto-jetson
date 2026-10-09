# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/gates/test_check_capability.py [Report|Fixtures|Failures]
"""Real-CLI gate tests: each case runs tools/gates/check_capability.py and asserts its exit code."""
from __future__ import annotations

import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

from support import FIXTURES, GATE_MODULES, REPORT, ROOT, GateCase
from test_check_capability_failures import FailureCases


PRIVACY_SCANS = (
    ('private IPv4', re.compile(r'\b(?:192\.168|10|172\.(?:1[6-9]|2\d|3[01]))\.\d{1,3}\.\d{1,3}\b')),
    ('MAC-derived interface', re.compile(r'\benx[0-9a-f]{12}\b')),
    ('email address', re.compile(r'[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}')),
    ('.local hostname', re.compile(r'\b[A-Za-z0-9-]+\.local\b')),
)


class Report(GateCase):
    """The real docs/capability-gate.json report behavior."""

    def test_report_continuation_exit2(self) -> None:
        self.assert_exit(REPORT, 2, 'owner-signed-reduced-scope')

    def test_report_final_release_exit1(self) -> None:
        self.assert_exit(REPORT, 1, 'provisional-unresolved', stage='final-release')

    def test_report_records_target_and_phone(self) -> None:
        data = json.loads(REPORT.read_text(encoding='utf-8'))
        self.assertEqual(data['target']['architecture'], 'aarch64')
        self.assertEqual(data['target']['l4t_release'], 'R39.2.1')
        self.assertIn('6.8.12-1021-tegra', data['target']['kernel_release'])
        self.assertEqual(data['phone']['android_version'], '16')
        self.assertEqual(data['phone']['android_auto_version'], '17.7.663654-release')

    def test_report_is_redacted_and_self_contained(self) -> None:
        text = REPORT.read_text(encoding='utf-8')
        for label, pattern in PRIVACY_SCANS:
            self.assertIsNone(pattern.search(text), msg=f'report leaks {label}')
        phone = json.loads(text)['phone']
        self.assertEqual(set(phone), {'model', 'android_version', 'android_auto_version', 'source'})

    def test_checker_self_contained_in_isolated_tree(self) -> None:
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as tmp:
            tree = Path(tmp)
            gates = tree / 'tools' / 'gates'
            gates.mkdir(parents=True)
            (tree / 'docs').mkdir()
            for name in GATE_MODULES:
                shutil.copy(ROOT / 'tools' / 'gates' / name, gates / name)
            shutil.copy(REPORT, tree / 'docs' / 'capability-gate.json')
            self.assertFalse((tree / '.omo').exists())
            result = subprocess.run(
                [sys.executable, '-B', str(gates / 'check_capability.py'), str(tree / 'docs' / 'capability-gate.json')],
                capture_output=True, text=True, check=False, cwd=tree)
        self.assertEqual(result.returncode, 2, msg=result.stdout + result.stderr)
        self.assertIn('owner-signed-reduced-scope', result.stdout + result.stderr)


class Fixtures(GateCase):
    """Happy fixtures: full-pass, fallback and reduced-scope continuation."""

    def test_good_provisional_exit0(self) -> None:
        self.assert_exit(FIXTURES / 'good_provisional.json', 0, 'RESULT')

    def test_good_reducedscope_exit2(self) -> None:
        self.assert_exit(FIXTURES / 'good_reducedscope.json', 2, 'owner-signed-reduced-scope')

    def test_good_provisional_release_stage_exit1(self) -> None:
        self.assert_exit(FIXTURES / 'good_provisional.json', 1, 'provisional-unresolved',
                         stage='final-release')

    def test_fixtures_use_synthetic_identity(self) -> None:
        for name in ('good_provisional.json', 'good_reducedscope.json'):
            phone = json.loads((FIXTURES / name).read_text(encoding='utf-8'))['phone']
            self.assertEqual(phone['model'], 'TEST-PHONE')
            self.assertIn('synthetic', phone['source'])


class Failures(FailureCases):
    """Expose failure tests through the existing CMake selector class name."""


if __name__ == '__main__':
    groups = {'report': Report, 'fixtures': Fixtures, 'failures': Failures}
    arg = sys.argv[1] if len(sys.argv) > 1 else None
    if arg is None or '.' not in arg:
        names = [arg.lower()] if arg else list(groups)
        suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(groups[name])
            for name in names)
    else:
        suite = unittest.defaultTestLoader.loadTestsFromName(arg, module=sys.modules['__main__'])
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
