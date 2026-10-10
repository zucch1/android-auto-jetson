# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/reference.py ROOT
"""Assert the synthetic test identity and fail-closed unknown-identity posture.

Replaces the former verbatim third-party reference extraction check: the
labeled public compatibility credential was removed on 2026-10-10 and no
credential copy may exist in the tree. This test requires that (1) the tree
holds zero credential copies and a fail-closed HU_KEY_PATH-only loading path,
(2) test identity is the generated synthetic neutral-DN self-signed credential
and never a third-party identity, and (3) unknown/absent identity fails closed
with a single redacted failure class.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(sys.argv[1])
    probe, policy = sys.argv[2], sys.argv[3]
    # Given: the sanitized tree and the shipped no-allowlist scanner.
    scan = subprocess.run([sys.executable, '-B', str(root / 'tools/tls/scan.py'), str(root)],
                          capture_output=True, text=True, check=True)
    assert 'zero-credential-copies' in scan.stdout, scan
    cryptor = (root / 'third_party/aasdk/src/Messenger/Cryptor.cpp').read_text()
    assert 'aa::tls::load_credentials()' in cryptor, 'loading path must be HU_KEY_PATH-only'
    assert 'cCertificate' not in cryptor and 'cPrivateKey' not in cryptor
    assert 'getenv' not in cryptor, 'no alternate implicit credential source'
    # When: generate the test identity, inspect it, and drive both fail-closed seams.
    with tempfile.TemporaryDirectory(prefix='tls-reference-') as directory:
        work = Path(directory)
        generation = subprocess.run(
            [sys.executable, '-B', str(root / 'tools/tls/synthetic_credential.py'), str(work)],
            capture_output=True, text=True, check=True)
        assert 'synthetic-credential-generated neutral-dn self-signed' in generation.stdout, generation
        described = subprocess.run(
            ['openssl', 'x509', '-in', str(work / 'synthetic-hu.crt'), '-noout', '-subject', '-issuer'],
            capture_output=True, text=True, check=True)
        identity = described.stdout
        assert 'android-auto-jetson-synthetic-hu' in identity, identity
        for third_party in ('JVC', 'Kenwood', 'Google Automotive'):
            assert third_party not in identity, identity
        subject = identity.splitlines()[0].split('subject=', 1)[-1]
        issuer = identity.splitlines()[1].split('issuer=', 1)[-1]
        assert subject == issuer, 'test identity must be self-signed'
        bundle = work / 'bundle.pem'
        bundle.write_bytes((work / 'synthetic-hu.crt').read_bytes() +
                           (work / 'synthetic-hu.key').read_bytes())
        bundle.chmod(0o600)
        loaded = subprocess.run([probe], env={**os.environ, 'HU_KEY_PATH': str(bundle)},
                                capture_output=True, text=True, check=False)
        assert loaded.returncode == 0 and loaded.stdout == 'tls-credential-loaded\n', loaded
        # Then: unknown credential identity and unknown phone identity both fail closed.
        denied = subprocess.run([probe], env={name: value for name, value in os.environ.items() if name != 'HU_KEY_PATH'},
                                capture_output=True, text=True, check=False)
        assert denied.returncode == 1 and denied.stderr.startswith('tls-credential-unset'), denied
        unknown_phone = subprocess.run([policy, 'unknown'], capture_output=True, text=True, check=False)
        assert unknown_phone.returncode == 0, unknown_phone
        assert unknown_phone.stdout.strip() == 'tls-phone-not-approved', unknown_phone
        absent_phone = subprocess.run([policy, 'absent'], capture_output=True, text=True, check=False)
        assert absent_phone.returncode == 0, absent_phone
        assert absent_phone.stdout.strip() == 'tls-phone-not-approved', absent_phone
    print('reference-posture: PASS synthetic-neutral-dn fail-closed-unknown-identity zero-credential-copies')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
