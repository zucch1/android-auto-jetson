# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/usb/assert_coverage.py build/host-dev
import json
import subprocess
import sys

inventory = subprocess.run(['ctest', '--test-dir', sys.argv[1], '--show-only=json-v1'],
                           capture_output=True, text=True, timeout=30, check=True)
registered = {test['name'] for test in json.loads(inventory.stdout)['tests']}
EXPECTED = {'usb_aoa_' + case for case in ('selection', 'handshake', 'libusb', 'sysfs',
                                           'hostenv', 'cycles', 'cli')}
missing = EXPECTED - registered
if missing:
    sys.exit(f'usb-aoa-coverage FAIL: missing {sorted(missing)}')
print(f'usb-aoa-coverage PASS: {len(EXPECTED)} required tests registered')
