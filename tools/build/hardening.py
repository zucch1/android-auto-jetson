# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/build/hardening.py BINARY [BINARY ...]
"""Fail unless each ELF executable has PIE, full RELRO, NX stack and stack canaries."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess


def readelf(binary: Path, *options: str) -> str:
    tool = shutil.which('readelf')
    if tool is None:
        raise RuntimeError('readelf is required')
    result = subprocess.run([tool, *options, str(binary)], capture_output=True, text=True,
                            check=False, timeout=30)
    if result.returncode != 0:
        raise RuntimeError(f'readelf failed for {binary}: {result.stderr.strip()}')
    return result.stdout


def check(binary: Path) -> tuple[str, ...]:
    """Return stable machine-readable hardening failures for one executable."""
    header = readelf(binary, '-h')
    dynamic = readelf(binary, '-d')
    segments = readelf(binary, '-W', '-l')
    symbols = readelf(binary, '-W', '-s')
    failures: list[str] = []
    if re.search(r'^\s*Type:\s+DYN\b', header, re.MULTILINE) is None:
        failures.append('not-pie')
    bind_now = (re.search(r'\(BIND_NOW\)', dynamic) is not None or
                re.search(r'\(FLAGS(?:_1)?\)[^\n]*\b(?:BIND_NOW|NOW)\b', dynamic) is not None)
    if 'GNU_RELRO' not in segments or not bind_now:
        failures.append('missing-full-relro')
    stack = [line for line in segments.splitlines() if re.match(r'^\s*GNU_STACK\b', line)]
    if not stack:
        failures.append('missing-gnu-stack')
    else:
        flags = [field for line in stack for field in line.split()[1:]
                 if re.fullmatch(r'[RWXE]+', field)]
        if not flags or any('E' in field or 'X' in field for field in flags):
            failures.append('executable-stack')
    if re.search(r'\b__stack_chk_(?:fail|guard)(?:@|\s|$)', symbols) is None:
        failures.append('missing-stack-protector')
    return tuple(failures)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binaries', metavar='BINARY', type=Path, nargs='+')
    args = parser.parse_args()
    failed = False
    for binary in args.binaries:
        try:
            failures = check(binary)
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            print(f'{binary}: FAIL hardening-check-error: {error}')
            failed = True
            continue
        if failures:
            print(f'{binary}: FAIL {",".join(failures)}')
            failed = True
        else:
            print(f'{binary}: PASS pie full-relro nx-stack stack-protector')
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())
