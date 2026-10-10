#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/dbus/assert_dbus_coverage.py BUILDDIR
#     python3 -B tests/dbus/assert_dbus_coverage.py --cross BUILDDIR
"""dbus_contract CTest inventory guard (task 21).

Native mode (default) fails when any required dbus_contract test is missing
from the build's CTest inventory, so `ctest -R dbus_contract` can never pass by
omission.

Cross mode (--cross) runs the dbus_contract_cross_build_inventory check on a
CMAKE_CROSSCOMPILING build: every IPC .cpp is in compile_commands.json, both
production library archives exist and hold AArch64 objects (readelf), and no
native aa_dbus_contract_test registration leaks into the cross configuration.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

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
    'dbus_contract_schema_neg_raw_delimiter_injection',
    'dbus_contract_schema_embedding_special_bytes',
    'dbus_contract_schema_embedding_crlf',
    'dbus_contract_coverage',
})

CROSS_CHECK = 'dbus_contract_cross_build_inventory'
LIBRARIES = ('libaa_ipc_control.a', 'libaa_ipc_dbus.a')
SOURCE_ROOT = Path(__file__).resolve().parents[2]


def inventory(build: Path) -> tuple[set[str], list[str]]:
    listing = subprocess.run(['ctest', '--test-dir', str(build), '--show-only=json-v1'],
                             capture_output=True, text=True, timeout=30, check=False)
    if listing.returncode != 0:
        return set(), []
    registered = {test['name'] for test in json.loads(listing.stdout)['tests']}
    commands = [part for test in json.loads(listing.stdout)['tests']
                for part in test.get('command', [])]
    return registered, commands


def check_native(build: Path) -> int:
    registered, _ = inventory(build)
    if not registered:
        print('dbus-coverage FAIL: inventory unavailable')
        return 1
    missing = sorted(EXPECTED - registered)
    if missing:
        print(f'dbus-coverage FAIL: missing {missing}')
        return 1
    print(f'dbus-coverage PASS: {len(EXPECTED)} required tests registered')
    return 0


def check_cross_inventory(build: Path) -> int:
    verdict = 0
    compile_commands = build / 'compile_commands.json'
    if not compile_commands.is_file():
        print(f'{CROSS_CHECK} FAIL: compile_commands.json missing under {build}')
        return 1
    entries = {Path(unit['file']).name: unit for unit in json.loads(compile_commands.read_text())}
    ipc_sources = sorted((SOURCE_ROOT / 'src' / 'ipc').rglob('*.cpp'))
    if not ipc_sources:
        print(f'{CROSS_CHECK} FAIL: no IPC sources found under {SOURCE_ROOT / "src" / "ipc"}')
        return 1
    for source in ipc_sources:
        if source.name not in entries:
            print(f'{CROSS_CHECK} FAIL: {source.name} absent from compile_commands.json')
            verdict = 1
    for archive_name in LIBRARIES:
        archives = list(build.rglob(archive_name))
        if len(archives) != 1:
            print(f'{CROSS_CHECK} FAIL: expected exactly one {archive_name}, found {len(archives)}')
            verdict = 1
            continue
        readelf = subprocess.run(['readelf', '-h', str(archives[0])],
                                 capture_output=True, text=True, timeout=30, check=False)
        machines = [line.split('Machine:', 1)[1].strip()
                    for line in readelf.stdout.splitlines() if 'Machine:' in line]
        if readelf.returncode != 0 or not machines:
            print(f'{CROSS_CHECK} FAIL: readelf cannot inspect {archives[0]}')
            verdict = 1
            continue
        foreign = sorted({machine for machine in machines if machine != 'AArch64'})
        if foreign:
            print(f'{CROSS_CHECK} FAIL: {archives[0].name} holds non-AArch64 objects {foreign}')
            verdict = 1
    registered, commands = inventory(build)
    if not registered and not commands:
        print(f'{CROSS_CHECK} FAIL: ctest inventory unavailable under {build}')
        return 1
    leaked = sorted(EXPECTED & registered)
    if leaked:
        print(f'{CROSS_CHECK} FAIL: native test registrations leaked into cross build: {leaked}')
        verdict = 1
    if any('aa_dbus_contract_test' in part for part in commands):
        print(f'{CROSS_CHECK} FAIL: aa_dbus_contract_test referenced by the cross inventory')
        verdict = 1
    if verdict == 0:
        print(f'{CROSS_CHECK} PASS: {len(ipc_sources)} IPC sources compiled, '
              f'{len(LIBRARIES)} AArch64 libraries, no native test registrations')
    return verdict


def main() -> int:
    if len(sys.argv) == 2:
        return check_native(Path(sys.argv[1]))
    if len(sys.argv) == 3 and sys.argv[1] == '--cross':
        return check_cross_inventory(Path(sys.argv[2]))
    print(f'usage: {sys.argv[0]} BUILDDIR | {sys.argv[0]} --cross BUILDDIR')
    return 2


if __name__ == '__main__':
    raise SystemExit(main())
