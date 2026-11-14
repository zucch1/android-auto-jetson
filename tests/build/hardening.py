# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/build/hardening.py
"""Negative and positive fixtures for the readelf ELF hardening gate."""
from __future__ import annotations

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.build.hardening import check


SOURCE = r'''
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) static int protected_copy(const char *input) {
    volatile char buffer[64];
    strncpy((char *)buffer, input, sizeof(buffer) - 1);
    buffer[sizeof(buffer) - 1] = '\0';
    return puts((const char *)buffer);
}

int main(int argc, char **argv) {
    return protected_copy(argc > 1 ? argv[1] : "fixture") < 0;
}
'''


def main() -> int:
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    common = ['-O2', '-fPIE', '-pie', '-Wl,-z,relro']
    cases = {
        'hardened': (common + ['-fstack-protector-strong', '-Wl,-z,now,-z,noexecstack'], ()),
        'non-pie': (['-O2', '-no-pie', '-fstack-protector-strong',
                     '-Wl,-z,relro,-z,now,-z,noexecstack'], ('not-pie',)),
        'partial-relro': (common + ['-fstack-protector-strong', '-Wl,-z,lazy,-z,noexecstack'],
                          ('missing-full-relro',)),
        'exec-stack': (common + ['-fstack-protector-strong', '-Wl,-z,now,-z,execstack'],
                       ('executable-stack',)),
        'no-stack-protector': (common + ['-fno-stack-protector', '-Wl,-z,now,-z,noexecstack'],
                               ('missing-stack-protector',)),
    }
    with tempfile.TemporaryDirectory(prefix='elf-hardening-') as directory:
        root = Path(directory)
        source = root / 'fixture.c'
        source.write_text(SOURCE)
        for name, (flags, expected) in cases.items():
            binary = root / name
            subprocess.run([*compiler, *flags, str(source), '-o', str(binary)], check=True,
                           capture_output=True, text=True, timeout=30)
            actual = check(binary)
            if name == 'hardened':
                assert not actual, actual
            else:
                assert expected[0] in actual, (name, expected, actual)
            print(f'PASS {name}: {actual or ("hardened",)}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
