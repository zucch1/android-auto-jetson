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
EXPECTED = {'wireless_' + case for case in ('policy', 'discovery', 'active', 'cli', 'window', 'bluez')} | {
    'wireless_regulatory_sections_fail_closed',
    'wireless_regulatory_countries_and_rules_fail_closed',
    'wireless_regulatory_restrictions_and_unknown_channels_fail_closed',
    'wireless_regulatory_canonical_confirmed_us_reaches_active',
    'wireless_psk_permissions_and_owner_prevent_contact',
    'wireless_psk_invalid_files_and_content_prevent_contact',
    'wireless_psk_opened_inode_survives_path_substitution',
    'wireless_psk_host_transfer_argv_and_report_are_secret_free',
    'wireless_psk_nm_secret_stdin_and_failure_diagnostics',
    'wireless_psk_nm_happy_argv_and_report_are_secret_free',
}
missing = EXPECTED - registered
if missing:
    sys.exit(f'wireless-coverage FAIL: missing {sorted(missing)}')
print(f'wireless-coverage PASS: {len(EXPECTED)} required tests registered')
