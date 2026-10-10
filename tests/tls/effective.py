# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/effective.py STAGE BUILD
"""Inspect the configured build's effective sources and generated install rules."""
from pathlib import Path
import re
import sys


def main() -> int:
    # Given: the exact content-addressed dependency stage selected at configure.
    stage, build = Path(sys.argv[1]) / 'aasdk', Path(sys.argv[2])
    marker = re.compile(rb'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----')
    # When: scan effective inputs with no reference or upstream exceptions.
    for path in stage.rglob('*'):
        if path.is_file():
            assert not marker.search(path.read_bytes()), path
    cryptor = (stage / 'src/Messenger/Cryptor.cpp').read_text()
    header = (stage / 'include/aasdk/Messenger/Cryptor.hpp').read_text()
    # Then: neither embedded credential symbols nor auto-install paths survive.
    assert all(symbol not in cryptor + header for symbol in ('cCertificate', 'cPrivateKey'))
    assert 'aa::tls::load_credentials()' in cryptor
    install = (build / 'aasdk/cmake_install.cmake').read_text()
    assert 'headunit.key' not in install and 'headunit.crt' not in install
    assert '/etc/aasdk' not in install
    print('effective-stage: PASS no-embedded-private-key no-bundled-install')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
