#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/trust/assert_trust_coverage.py BUILDDIR
#     python3 -B tests/trust/assert_trust_coverage.py --cross BUILDDIR
"""trust_store CTest inventory guard (task 27 fix-forward).

Native mode (default) fails when any required trust_store test is missing
from the build's CTest inventory, so `ctest -R '^trust_'` can never pass by
omission.

Cross mode (--cross) inspects a CMAKE_CROSSCOMPILING build: every trust
production source is in compile_commands.json, the aa_trust archive exists and
holds only AArch64 objects (readelf), and no native trust-test registration or
test executable leaks into the cross configuration.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

EXPECTED = frozenset({
    'trust_store_store',
    'trust_store_permissions',
    'trust_store_migration',
    'trust_store_schema',
    'trust_store_policy',
    'trust_store_expiry',
    'trust_store_redaction',
    'trust_store_control_flow',
    'trust_store_bus',
})

CROSS_CHECK = 'trust_store_cross_build_inventory'
LIBRARY = 'libaa_trust.a'
TEST_EXECUTABLES = ('aa_trust_store_test', 'aa_trust_bus_test')
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
        print('trust-coverage FAIL: inventory unavailable')
        return 1
    missing = sorted(EXPECTED - registered)
    if missing:
        print(f'trust-coverage FAIL: missing {missing}')
        return 1
    print(f'trust-coverage PASS: {len(EXPECTED)} required tests registered')
    return 0


def check_cross_inventory(build: Path) -> int:
    verdict = 0
    compile_commands = build / 'compile_commands.json'
    if not compile_commands.is_file():
        print(f'{CROSS_CHECK} FAIL: compile_commands.json missing under {build}')
        return 1
    entries = {Path(unit['file']).name: unit for unit in json.loads(compile_commands.read_text())}
    trust_sources = sorted((SOURCE_ROOT / 'src' / 'trust').rglob('*.cpp'))
    if not trust_sources:
        print(f'{CROSS_CHECK} FAIL: no trust sources found under {SOURCE_ROOT / "src" / "trust"}')
        return 1
    for source in trust_sources:
        if source.name not in entries:
            print(f'{CROSS_CHECK} FAIL: {source.name} absent from compile_commands.json')
            verdict = 1
    archives = list(build.rglob(LIBRARY))
    if len(archives) != 1:
        print(f'{CROSS_CHECK} FAIL: expected exactly one {LIBRARY}, found {len(archives)}')
        verdict = 1
    else:
        readelf = subprocess.run(['readelf', '-h', str(archives[0])],
                                 capture_output=True, text=True, timeout=30, check=False)
        machines = [line.split('Machine:', 1)[1].strip()
                    for line in readelf.stdout.splitlines() if 'Machine:' in line]
        if readelf.returncode != 0 or not machines:
            print(f'{CROSS_CHECK} FAIL: readelf cannot inspect {archives[0]}')
            verdict = 1
        else:
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
    for executable in TEST_EXECUTABLES:
        if any(executable in part for part in commands):
            print(f'{CROSS_CHECK} FAIL: {executable} referenced by the cross inventory')
            verdict = 1
    if verdict == 0:
        print(f'{CROSS_CHECK} PASS: {len(trust_sources)} trust sources compiled, '
              f'{LIBRARY} is AArch64-only, no native test registrations')
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
