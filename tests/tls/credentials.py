# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/credentials.py ROOT PROBE CASE
"""Drive HU_KEY_PATH failures through a real credential-loading process."""
from enum import StrEnum
from pathlib import Path
import base64
import os
import subprocess
import sys
import tempfile
from typing import assert_never


class Case(StrEnum):
    VALID = 'valid'
    UNSET = 'unset'
    EMPTY = 'empty'
    UNREADABLE = 'unreadable'
    INSECURE = 'insecure'
    MALFORMED = 'malformed'
    MISMATCH = 'mismatch'
    SYMLINK = 'symlink'
    HARDLINK = 'hardlink'
    TRAILING = 'trailing'


def main() -> int:
    root, probe, case = Path(sys.argv[1]), sys.argv[2], Case(sys.argv[3])
    # Given: only the existing public reference private key; no key generation.
    reference = root / 'third_party/compat-credentials'
    certificate = (reference / 'headunit.crt').read_bytes()
    key = (reference / 'headunit.key').read_bytes()
    environment = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix='tls-credential-') as directory:
        bundle = Path(directory) / 'sensitive-name.pem'
        bundle.write_bytes(certificate + key)
        bundle.chmod(0o600)
        environment['HU_KEY_PATH'] = str(bundle)
        expected = ''
        match case:
            case Case.VALID:
                pass
            case Case.UNSET:
                del environment['HU_KEY_PATH']
                expected = 'unset'
            case Case.EMPTY:
                environment['HU_KEY_PATH'] = ''
                expected = 'unset'
            case Case.UNREADABLE:
                environment['HU_KEY_PATH'] = str(bundle.with_name('absent.pem'))
                expected = 'unreadable'
            case Case.INSECURE:
                bundle.chmod(0o640)
                expected = 'insecure'
            case Case.MALFORMED:
                bundle.write_bytes(b'not PEM\n')
                expected = 'malformed'
            case Case.MISMATCH:
                # Mutate only the public modulus in the reference certificate DER.
                lines = certificate.splitlines()
                der = bytearray(base64.b64decode(b''.join(lines[1:-1])))
                modulus = bytes.fromhex('cf75d6636751fc7e589b71ba4d3a18ee')
                offset = der.index(modulus)
                der[offset + 8] ^= 1
                encoded = base64.b64encode(der)
                pem = b'\n'.join([lines[0], *(encoded[i:i + 64] for i in range(0, len(encoded), 64)), lines[-1]]) + b'\n'
                bundle.write_bytes(pem + key)
                expected = 'mismatch'
            case Case.SYMLINK:
                link = bundle.with_name('link.pem')
                link.symlink_to(bundle)
                environment['HU_KEY_PATH'] = str(link)
                expected = 'insecure'
            case Case.HARDLINK:
                bundle.with_name('hardlink.pem').hardlink_to(bundle)
                expected = 'insecure'
            case Case.TRAILING:
                bundle.write_bytes(certificate + key + b'unexpected\n')
                expected = 'malformed'
            case unreachable:
                assert_never(unreachable)
        # When: load through the same component used by effective Cryptor.
        result = subprocess.run([probe], env=environment, capture_output=True, text=True, check=False)
        # Then: nonzero failures expose only a class, never path or material.
        assert result.returncode == (1 if expected else 0), result
        if expected:
            assert result.stderr.startswith(f'tls-credential-{expected}'), result
            assert result.stdout == '', result
        else:
            assert result.stdout == 'tls-credential-loaded\n', result
        assert directory not in result.stderr and 'sensitive-name' not in result.stderr, result
        assert key.decode() not in result.stderr, result
        print(f'credential-case={case} probe_exit={result.returncode} redacted=yes')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
