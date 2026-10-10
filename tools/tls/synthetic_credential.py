# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/tls/synthetic_credential.py OUTDIR
"""Generate the synthetic, neutral-DN, self-signed head-unit test credential.

Test material only. The output is generated fresh into OUTDIR (never into the
source tree) and is clearly labeled synthetic: a neutral subject with no
third-party identity, self-signed, valid for the test window. It is NOT the
historical public compatibility credential and must never be described as one.
No credential bytes are committed anywhere in this repository; this script is
the only in-tree source of test identity, and it needs the `openssl` CLI.

Determinism: the distinguished name, key type/size and validity are fixed by
this script. The RSA keypair itself is freshly generated per run (OpenSSL has
no fixed-seed keygen); callers must not assume bit-identical output.
"""
from pathlib import Path
import re
import subprocess
import sys

# Neutral DN: synthetic test fixture identity, no real-world organization.
SUBJECT = ('/CN=android-auto-jetson-synthetic-hu'
           '/OU=SYNTHETIC-TEST-FIXTURE'
           '/O=android-auto-jetson')
CERT_NAME = 'synthetic-hu.crt'
KEY_NAME = 'synthetic-hu.key'


def generate(out_directory: Path) -> tuple[Path, Path]:
    """Write a labeled synthetic certificate and private key; return paths."""
    out_directory.mkdir(parents=True, exist_ok=True)
    certificate = out_directory / CERT_NAME
    key = out_directory / KEY_NAME
    for path in (certificate, key):
        if path.exists():
            path.unlink()
    result = subprocess.run(
        ['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
         '-sha256', '-days', '3650', '-subj', SUBJECT,
         '-keyout', str(key), '-out', str(certificate)],
        capture_output=True, text=True, check=False)
    if result.returncode != 0:
        raise RuntimeError('synthetic-credential-generation-failed')
    key.chmod(0o600)
    described = subprocess.run(
        ['openssl', 'x509', '-in', str(certificate), '-noout', '-subject', '-issuer'],
        capture_output=True, text=True, check=False)
    if described.returncode != 0:
        raise RuntimeError('synthetic-credential-unreadable')
    identity = re.sub(r'\s+', '', described.stdout)
    for marker in ('JVC', 'Kenwood', 'GoogleAutomotive', 'jvc', 'kenwood'):
        if marker in identity:
            raise RuntimeError('synthetic-credential-identity-leak')
    for component in ('CN=android-auto-jetson-synthetic-hu',
                      'OU=SYNTHETIC-TEST-FIXTURE', 'O=android-auto-jetson'):
        if component not in identity:
            raise RuntimeError('synthetic-credential-subject-mismatch')
    if identity.count('CN=android-auto-jetson-synthetic-hu') != 2:
        raise RuntimeError('synthetic-credential-not-self-signed')
    return certificate, key


def main() -> int:
    if len(sys.argv) != 2:
        print('usage: synthetic_credential.py OUTDIR', file=sys.stderr)
        return 2
    try:
        certificate, key = generate(Path(sys.argv[1]))
    except (OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(f'synthetic-credential-generated neutral-dn self-signed cert={certificate.name} key={key.name}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
