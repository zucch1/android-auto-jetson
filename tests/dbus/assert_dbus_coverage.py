#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/dbus/assert_dbus_coverage.py BUILDDIR
"""dbus_contract CTest inventory guard (task 21).

Fails when any required dbus_contract test is missing from the build's CTest
inventory, so `ctest -R dbus_contract` can never pass by omission.
"""
from __future__ import annotations

import json
import subprocess
import sys

EXPECTED = frozenset({
    'dbus_contract_control',
    'dbus_contract_access',
    'dbus_contract_pairing',
    'dbus_contract_introspection',
    'dbus_contract_dispatch',
    'dbus_contract_bus',
    'dbus_contract_boundary',
    'dbus_contract_spoof',
    'dbus_contract_schema_real_check',
    'dbus_contract_schema_real_generate',
    'dbus_contract_schema_neg_malformed_xml',
    'dbus_contract_schema_neg_unsupported_major',
    'dbus_contract_schema_neg_overflow_major',
    'dbus_contract_schema_neg_high_rate_payload',
    'dbus_contract_schema_neg_missing_method',
    'dbus_contract_schema_neg_bad_signature',
    'dbus_contract_schema_neg_semver_mismatch',
    'dbus_contract_schema_neg_unexpected_method',
    'dbus_contract_schema_neg_duplicate_method',
    'dbus_contract_schema_neg_bogus_direction',
    'dbus_contract_schema_neg_masking_duplicate',
    'dbus_contract_schema_neg_multi_type_method',
    'dbus_contract_schema_neg_multi_type_signal',
    'dbus_contract_coverage',
})


def main() -> int:
    inventory = subprocess.run(['ctest', '--test-dir', sys.argv[1], '--show-only=json-v1'],
                               capture_output=True, text=True, timeout=30, check=False)
    if inventory.returncode != 0:
        print('dbus-coverage FAIL: inventory unavailable')
        return 1
    registered = {test['name'] for test in json.loads(inventory.stdout)['tests']}
    missing = sorted(EXPECTED - registered)
    if missing:
        print(f'dbus-coverage FAIL: missing {missing}')
        return 1
    print(f'dbus-coverage PASS: {len(EXPECTED)} required tests registered')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
