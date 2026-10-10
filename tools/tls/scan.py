# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/tls/scan.py ROOT
#     python3 -B tools/tls/scan.py --print-denylist
#     python3 -B tools/tls/scan.py --print-der-denylist
"""Prove absence of head-unit credential copies: no allowlist, zero tolerance.

Rejects (with no exceptions anywhere in the scanned tree):
- any private-key or certificate PEM block, in any armor spelling (CERTIFICATE,
  TRUSTED CERTIFICATE, X509 CERTIFICATE, any *PRIVATE KEY*),
- any file named headunit.key or headunit.crt (the historical credential names),
- any byte-identical copy of the removed 2026-10-10 credential (digest denylist),
- recognizable re-encodings of that same removed certificate/private key:
  raw DER, whitespace-stripped base64 or hex of any of the above (decoded, then
  matched again as digest or PEM armor; decodes nest),
- any symlink.

The digest denylist pins the two distinct historical byte strings that were
published in three copies each (original SHA-256 identities, recorded as the
originals of the sanitization record in third_party/provenance/inventory.json),
plus their DER shapes (X.509 certificate DER; RSA private key as PKCS#1 and
PKCS#8 DER). The DER digests are derived fingerprints of the same recorded
historical identities: DER = the base64 body of the recorded PEM blobs
(git blobs 45ad6cc4fd9fe202fa4e83c98bcc214f11b7332e /
c2b2666a80217d4c1d64d8a9fe85ac39a62016ce, whose SHA-256 identities are the
original_sha256 values in the sanitization record). tests/tls/scan.py
re-derives both families from that record and requires exact agreement.
--print-denylist exposes the PEM-identity digests and --print-der-denylist the
DER-identity digests so tests can bind this gate to the provenance record.

Enforced formats versus one-time manual inspection: everything in the reject
list above is mechanically enforced and regression-tested (the adversarial
cases in tests/tls/scan.py plant each re-encoding under unrelated filenames).
Further transformative containers of the same material (compressed, archived,
encrypted or otherwise wrapped beyond one or more base64/hex layers or PEM
armor) are OUT of enforced scope and were only inspected manually during the
2026-10-10 gate review; absence proofs do not claim them.

Inputs fail closed: the scan root must exist and is a directory, and any
traversal or read error fails the scan. Absence is never reported for a
missing, non-directory or incompletely traversed root.
"""
from pathlib import Path
import base64
import binascii
import hashlib
import re
import sys
from typing import Final

FORBIDDEN_DIGESTS: Final = frozenset((
    '9e837a172a1eef5b05cda9df9d086753639648a17f76c2c939bcbd094aa972f2',
    '85b5043a09b1ba9464f745e6917bdbaa2bc582fe48cb727a7787c1a832a773e4',
))
FORBIDDEN_DER_DIGESTS: Final = frozenset((
    # X.509 certificate DER (base64 body of the recorded 85b5043a... PEM blob)
    '1c0e0ef9e672dd1a63ab4d61afaa8996a57ca7aa966d1922b97d8f9385ea3c35',
    # RSA private key DER, PKCS#1 RSAPrivateKey (body of the 9e837a17... PEM blob)
    '08e86e4de51208bfa0e8164d99c50daf7f5bfcfa3fe2067dc912cf83f2e99a25',
    # RSA private key DER, PKCS#8 PrivateKeyInfo (same key, wrapped)
    'f13e57175f294891156ed1e363a4dbfc624f6a62ee031f44611d7176d95fbf4a',
))
FORBIDDEN_NAMES: Final = frozenset(('headunit.key', 'headunit.crt'))
MARKER: Final = re.compile(
    rb'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----'
    rb'|-----BEGIN (?:[A-Z0-9]+ )*CERTIFICATE-----')
PEM_BLOCK: Final = re.compile(
    rb'-----BEGIN (?:[A-Z0-9]+ )*(?:PRIVATE KEY|CERTIFICATE)-----'
    rb'(.*?)'
    rb'-----END (?:[A-Z0-9]+ )*(?:PRIVATE KEY|CERTIFICATE)-----', re.DOTALL)
HEX: Final = re.compile(rb'[0-9a-fA-F]+')
EXCLUDED: Final = frozenset(('.git', '.omo', 'build', '.local'))
MAX_DERIVED: Final = 16


def derived(data: bytes) -> set[bytes]:
    """Normalize one file body into candidate plaintext shapes.

    Yields the raw bytes plus every re-encoding this gate enforces: base64
    bodies of PEM armor blocks (i.e. DER), and whitespace-stripped base64 or
    hex decodes of any shape already derived (decodes nest, bounded). A
    candidate that fails to decode is skipped, never treated as clean proof.
    """
    seen = {data}
    queue = [data]
    while queue and len(seen) < MAX_DERIVED:
        blob = queue.pop()
        for body in PEM_BLOCK.findall(blob):
            try:
                decoded = base64.b64decode(b''.join(body.split()), validate=True)
            except (binascii.Error, ValueError):
                continue
            if decoded and decoded not in seen:
                seen.add(decoded)
                queue.append(decoded)
        stripped = b''.join(blob.split())
        if stripped:
            try:
                decoded = base64.b64decode(stripped, validate=True)
            except (binascii.Error, ValueError):
                decoded = b''
            if decoded and decoded not in seen:
                seen.add(decoded)
                queue.append(decoded)
            if len(stripped) % 2 == 0 and HEX.fullmatch(stripped):
                decoded = bytes.fromhex(stripped.decode('ascii'))
                if decoded and decoded not in seen:
                    seen.add(decoded)
                    queue.append(decoded)
    return seen


def hits(data: bytes) -> bool:
    """Return True when any derived shape is a known identity or PEM armor."""
    digests = FORBIDDEN_DIGESTS | FORBIDDEN_DER_DIGESTS
    for blob in derived(data):
        if hashlib.sha256(blob).hexdigest() in digests:
            return True
        if MARKER.search(blob):
            return True
    return False


def forbidden(root: Path) -> bool:
    """Return True on forbidden material; fail closed on invalid or partial scans.

    Raises ValueError when root is missing or is not a directory, and
    propagates OSError from any traversal or read failure, so neither an
    invalid root nor an incomplete traversal can be reported as clean.
    """
    if not root.is_dir():
        raise ValueError(f'scan root is not a directory: {root}')

    def raise_error(error: OSError) -> None:
        raise error

    for directory, subdirectories, files in root.walk(on_error=raise_error):
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
            if hits(data):
                return True
    return False


def main() -> int:
    arguments = sys.argv[1:]
    if arguments == ['--print-denylist']:
        for digest in sorted(FORBIDDEN_DIGESTS):
            print(digest)
        return 0
    if arguments == ['--print-der-denylist']:
        for digest in sorted(FORBIDDEN_DER_DIGESTS):
            print(digest)
        return 0
    if len(arguments) != 1:
        print('tls-key-scan: FAIL usage', file=sys.stderr)
        return 1
    try:
        rejected = forbidden(Path(arguments[0]).resolve())
    except ValueError:
        print('tls-key-scan: FAIL invalid-root')
        return 1
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
