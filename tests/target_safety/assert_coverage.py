# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/assert_coverage.py build/host-dev
"""Every main-guard suite in this directory is CTest-registered or carries an exclusion reason."""
import json
from pathlib import Path
import subprocess
import sys

LOC_GATE = ('pre-existing failure at base: tools/target_safety/qualification.py rejects '
            'over-250-pure-LOC modules (snapshot_diff, snapshot_gate_e2e, snapshot_diff_compare, '
            'snapshot_gate_review_fixes); tracked as a separate correction')
EXCLUDED = {
    'ceiling_argv': LOC_GATE,
    'ceiling_attempt': LOC_GATE,
    'ceiling_capacity_route': LOC_GATE,
    'ceiling_capture_paths': LOC_GATE,
    'ceiling_defects': LOC_GATE,
    'ceiling_durable': LOC_GATE,
    'ceiling_policy': LOC_GATE,
    'ceiling_qa': LOC_GATE,
    'ceiling_two_links_route': LOC_GATE,
    'receipt': LOC_GATE,
    'scenarios': LOC_GATE,
    'sizing_route': LOC_GATE,
    'sizing_sqlite_route': LOC_GATE,
    'ceiling_capture': 'requires a capture directory argument; driven by ceiling tooling, not standalone',
}

inventory = subprocess.run(['ctest', '--test-dir', sys.argv[1], '--show-only=json-v1'],
                           capture_output=True, text=True, timeout=30, check=True)
registered = {test['name'] for test in json.loads(inventory.stdout)['tests']}
suites = {path.stem for path in Path(__file__).resolve().parent.glob('*.py')
          if 'if __name__' in path.read_text()} - {'assert_coverage'}
problems = []
for suite in sorted(suites):
    test_name = f'target_safety_{suite}'
    if suite in EXCLUDED:
        if test_name in registered:
            problems.append(f'{suite}: excluded but registered')
    elif test_name not in registered:
        problems.append(f'{suite}: neither registered nor excluded')
for name in sorted(EXCLUDED):
    if name not in suites:
        problems.append(f'{name}: exclusion entry without a suite file')
if problems:
    sys.exit('target-safety-coverage FAIL: ' + '; '.join(problems))
print(f'target-safety-coverage PASS: {len(suites) - len(EXCLUDED)} registered, '
      f'{len(EXCLUDED)} explicitly excluded')
