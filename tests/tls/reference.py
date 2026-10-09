# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/reference.py ROOT
"""Require verbatim public reference PEM extraction, not a generated credential."""
from pathlib import Path
import re
import sys


def main() -> int:
    root = Path(sys.argv[1])
    # Given: immutable upstream C++ string literals.
    source = (root / 'third_party/aasdk/src/Messenger/Cryptor.cpp').read_text()
    for symbol, name in (('cCertificate', 'headunit.crt'), ('cPrivateKey', 'headunit.key')):
        literal = re.search(rf'Cryptor::{symbol} = "(.*?)";', source, re.S)
        assert literal is not None
        expected = literal[1].replace('\\\n', '').replace('\\n', '\n').encode()
        # When: inspect the retained reference bytes.
        actual = (root / 'third_party/compat-credentials' / name).read_bytes()
        # Then: only the exact existing public upstream credential is retained.
        assert actual == expected, name
    print('reference-extraction: PASS verbatim-public-upstream')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
