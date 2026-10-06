# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/tls/scan.py ROOT
"""Reject private-key PEM markers except exact public-reference/upstream blobs."""
from pathlib import Path
import hashlib
import re
import sys
from typing import Final

# These two upstream exceptions cannot smuggle changes: the task-2 566-file
# provenance gate and these SHA-256 identities both require the original bytes.
PINNED: Final = {
    'third_party/compat-credentials/headunit.key':
        '9e837a172a1eef5b05cda9df9d086753639648a17f76c2c939bcbd094aa972f2',
    'third_party/aasdk/cert/headunit.key':
        '9e837a172a1eef5b05cda9df9d086753639648a17f76c2c939bcbd094aa972f2',
    'third_party/aasdk/src/Messenger/Cryptor.cpp':
        'af6d9f58d135229a0ef1de3a09caf59b32a3cf064c0198d4a82626ca2fc2be9d',
}
CERT_PATH: Final = 'third_party/compat-credentials/headunit.crt'
CERT_DIGEST: Final = '85b5043a09b1ba9464f745e6917bdbaa2bc582fe48cb727a7787c1a832a773e4'
MARKER: Final = re.compile(rb'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----')
EXCLUDED: Final = frozenset(('.git', '.omo', 'build', '.local', '__pycache__'))


def forbidden(root: Path) -> bool:
    """Scan source files; generated build/test fixtures are a separate stage gate."""
    certificate = root / CERT_PATH
    if (certificate.is_symlink() or not certificate.is_file() or
            hashlib.sha256(certificate.read_bytes()).hexdigest() != CERT_DIGEST):
        return True
    for path in root.rglob('*'):
        relative = path.relative_to(root)
        if any(part in EXCLUDED for part in relative.parts):
            continue
        if path.is_symlink():
            return True
        if path.is_file():
            data = path.read_bytes()
            pinned = PINNED.get(relative.as_posix())
            digest = hashlib.sha256(data).hexdigest()
            if pinned is not None and digest != pinned:
                return True
            if MARKER.search(data) and digest != pinned:
                return True
    return False


def main() -> int:
    try:
        rejected = forbidden(Path(sys.argv[1]).resolve())
    except OSError:
        print('tls-key-scan: FAIL unreadable-source')
        return 1
    if rejected:
        print('tls-key-scan: FAIL forbidden-private-key-marker')
        return 1
    print('tls-key-scan: PASS digest-pinned-public-reference-only')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
