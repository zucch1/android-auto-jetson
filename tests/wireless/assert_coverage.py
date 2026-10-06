# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wireless/assert_coverage.py build/host-dev
import json
import subprocess
import sys

inventory = subprocess.run(['ctest', '--test-dir', sys.argv[1], '--show-only=json-v1'],
                           capture_output=True, text=True, timeout=30, check=True)
registered = {test['name'] for test in json.loads(inventory.stdout)['tests']}
expected = {'wireless_' + case for case in ('policy', 'discovery', 'active', 'cli', 'window', 'bluez')}
missing = expected - registered
if missing:
    sys.exit(f'wireless-coverage FAIL: missing {sorted(missing)}')
print(f'wireless-coverage PASS: {len(expected)} required tests registered')
