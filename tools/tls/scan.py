# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/tls/scan.py ROOT
#     python3 -B tools/tls/scan.py --print-denylist
"""Prove absence of head-unit credential copies: no allowlist, zero tolerance.

Rejects (with no exceptions anywhere in the scanned tree):
- any private-key or certificate PEM block (no credential material is committed),
- any file named headunit.key or headunit.crt (the historical credential names),
- any byte-identical copy of the removed 2026-10-10 credential (digest denylist),
- any symlink.

The digest denylist pins the two distinct historical byte strings that were
published in three copies each (original SHA-256 identities, recorded as the
originals of the sanitization record in third_party/provenance/inventory.json).
--print-denylist exposes those digests so tests can bind this gate to the
provenance record.
"""
from pathlib import Path
import hashlib
import re
import sys
from typing import Final

FORBIDDEN_DIGESTS: Final = frozenset((
    '9e837a172a1eef5b05cda9df9d086753639648a17f76c2c939bcbd094aa972f2',
    '85b5043a09b1ba9464f745e6917bdbaa2bc582fe48cb727a7787c1a832a773e4',
))
FORBIDDEN_NAMES: Final = frozenset(('headunit.key', 'headunit.crt'))
MARKER: Final = re.compile(
    rb'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----|-----BEGIN[ ]CERTIFICATE-----')
EXCLUDED: Final = frozenset(('.git', '.omo', 'build', '.local'))


def forbidden(root: Path) -> bool:
    for directory, subdirectories, files in root.walk():
        if directory == root:
            subdirectories[:] = [name for name in subdirectories if name not in EXCLUDED]
            files = [name for name in files if name not in EXCLUDED]
        for name in files:
            path = directory / name
            if path.is_symlink():
                return True
            if name in FORBIDDEN_NAMES:
                return True
            data = path.read_bytes()
            if hashlib.sha256(data).hexdigest() in FORBIDDEN_DIGESTS:
                return True
            if MARKER.search(data):
                return True
    return False


def main() -> int:
    arguments = sys.argv[1:]
    if arguments == ['--print-denylist']:
        for digest in sorted(FORBIDDEN_DIGESTS):
            print(digest)
        return 0
    if len(arguments) != 1:
        print('tls-key-scan: FAIL usage', file=sys.stderr)
        return 1
    try:
        rejected = forbidden(Path(arguments[0]).resolve())
    except OSError:
        print('tls-key-scan: FAIL unreadable-source')
        return 1
    if rejected:
        print('tls-key-scan: FAIL credential-material-present')
        return 1
    print('tls-key-scan: PASS zero-credential-copies no-allowlist')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
