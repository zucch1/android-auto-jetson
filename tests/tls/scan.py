# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/scan.py ROOT [--negative | --reencodings | --invalid-root CASE]
"""Assert ZERO credential copies remain and that the scanner still detects plants.

Positive mode runs the shipped no-allowlist marker/digest/name scan against the
real source tree (it must prove absence), and binds the scanner digest denylist
to the sanitization record in third_party/provenance/inventory.json. Negative
mode plants credential-shaped material in a private fixture and requires
rejection: private-key PEM blocks, certificate PEM blocks, the historical
credential file names and planted copies under source-like directories.
Reencodings mode is the adversarial bypass regression: it re-derives the
recorded historical certificate/private key from the git blobs pinned in the
sanitization record (read-only; the bytes match the recorded original_sha256
identities and are never written into the source tree), then plants each
recognized re-encoding -- openssl x509 -trustout TRUSTED CERTIFICATE PEM, raw
DER, base64 of the original PEM, base64 of the historical key PEM, hex of the
certificate DER, and the PKCS#1/PKCS#8 key DER shapes -- under unrelated
filenames in a private fixture, and requires rejection (exit nonzero, no PASS)
for every one of them. A benign control plant must still scan clean.
Invalid-root mode feeds the scanner broken scan inputs -- a nonexistent root,
a regular-file root and a tree with an unreadable descendant directory -- and
requires nonzero exit with no PASS proof for any of them: invalid inputs must
never produce a false absence proof.
"""
from pathlib import Path
import base64
import hashlib
import json
import subprocess
import sys
import tempfile


def run(scanner: Path, target: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, '-B', str(scanner), str(target)],
                          capture_output=True, text=True, check=False)


def prove_absence(root: Path) -> None:
    scanner = root / 'tools/tls/scan.py'
    result = run(scanner, root)
    assert result.returncode == 0, result
    assert result.stdout == 'tls-key-scan: PASS zero-credential-copies no-allowlist\n', result
    inventory = json.loads((root / 'third_party/provenance/inventory.json').read_bytes())
    removed = {record['original_sha256'] for record in inventory['downstream_patches']
               if record['resulting'] == 'deleted'}
    denied = set(subprocess.run([sys.executable, '-B', str(scanner), '--print-denylist'],
                                capture_output=True, text=True, check=True).stdout.split())
    assert denied == removed, (denied, removed)
    assert all((root / name).exists() is False for name in (
        'third_party/compat-credentials/headunit.key',
        'third_party/compat-credentials/headunit.crt',
        'third_party/aasdk/cert/headunit.key',
        'third_party/aasdk/cert/headunit.crt'))


def plant_rejections(root: Path) -> None:
    scanner = root / 'tools/tls/scan.py'
    synthetic = root / 'tools/tls/synthetic_credential.py'
    with tempfile.TemporaryDirectory(prefix='tls-scan-') as directory:
        fixture = Path(directory)
        generation = subprocess.run([sys.executable, '-B', str(synthetic), str(fixture)],
                                    capture_output=True, text=True, check=True)
        assert 'synthetic-credential-generated' in generation.stdout, generation
        key = (fixture / 'synthetic-hu.key').read_bytes()
        certificate = (fixture / 'synthetic-hu.crt').read_bytes()
        (fixture / 'synthetic-hu.key').unlink()
        (fixture / 'synthetic-hu.crt').unlink()
        cases = {
            'planted.pem': key,
            'tools/build/planted.pem': key,
            'src/__pycache__/planted.pem': key,
            'third_party/aasdk/planted.pem': key,
            'third_party/compat-credentials/injected.key': key,
            'headunit.key': key,
            'anywhere/headunit.crt': certificate,
            'wrapped-cert.txt': b'prologue\n' + certificate + b'epilogue\n',
        }
        for name, payload in cases.items():
            target = fixture / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
            result = run(scanner, fixture)
            assert result.returncode == 1, (name, result)
            assert result.stdout == 'tls-key-scan: FAIL credential-material-present\n', (name, result)
            target.unlink()
        historical = denied_digests(scanner)
        payload = b'synthetic stand-in for a digest-checked copy\n'
        assert hashlib.sha256(payload).hexdigest() not in historical, payload
        marker = fixture / 'digest-branch-probe.txt'
        marker.write_bytes(payload)
        assert run(scanner, fixture).returncode == 0
        marker.unlink()


def invalid_root(root: Path, case: str) -> None:
    scanner = root / 'tools/tls/scan.py'
    with tempfile.TemporaryDirectory(prefix='tls-scan-input-') as directory:
        fixture = Path(directory)
        match case:
            case 'nonexistent-root':
                target = fixture / 'absent-root'
                expected = 'tls-key-scan: FAIL invalid-root\n'
            case 'regular-file-root':
                target = fixture / 'regular-file'
                target.write_bytes(b'not a scan root\n')
                expected = 'tls-key-scan: FAIL invalid-root\n'
            case 'unreadable-descendant':
                target = fixture / 'tree'
                (target / 'locked').mkdir(parents=True)
                (target / 'locked' / 'hidden.txt').write_text('unscannable\n')
                (target / 'visible.txt').write_text('visible\n')
                (target / 'locked').chmod(0)
                expected = 'tls-key-scan: FAIL unreadable-source\n'
            case unknown:
                raise AssertionError(f'unknown invalid-root case: {unknown}')
        try:
            result = run(scanner, target)
        finally:
            if case == 'unreadable-descendant':
                (target / 'locked').chmod(0o700)
        assert result.returncode != 0, (case, result)
        assert result.stdout == expected, (case, result)
        assert 'PASS' not in result.stdout and 'PASS' not in result.stderr, (case, result)


def denied_digests(scanner: Path) -> set[str]:
    result = subprocess.run([sys.executable, '-B', str(scanner), '--print-denylist'],
                            capture_output=True, text=True, check=True)
    return set(result.stdout.split())


def historical_identity(root: Path) -> tuple[bytes, bytes]:
    """Re-derive the recorded historical certificate and key bytes read-only.

    The sanitization record pins each removed copy's original git blob OID and
    original_sha256; those blobs remain in repository history (the documented
    known exposure). This reads them with git cat-file, proves the bytes match
    the recorded identities, and never writes them into the source tree. A
    shallow clone without the blobs fails closed here rather than skipping.
    """
    inventory = json.loads((root / 'third_party/provenance/inventory.json').read_bytes())
    identities: dict[str, bytes] = {}
    for record in inventory['downstream_patches']:
        if record['resulting'] != 'deleted' or record['original_sha256'] in identities:
            continue
        blob = subprocess.run(
            ['git', '-C', str(root), 'cat-file', 'blob', record['original_git_blob_oid']],
            capture_output=True, check=False)
        assert blob.returncode == 0, (
            'historical blob unavailable (full history required): '
            f"{record['original_git_blob_oid']}", blob.stderr)
        data = blob.stdout
        assert hashlib.sha256(data).hexdigest() == record['original_sha256'], record
        identities[record['original_sha256']] = data
    assert len(identities) == 2, identities.keys()
    certificate = next(body for body in identities.values()
                       if body.startswith(b'-----BEGIN CERTIFICATE'))
    key = next(body for body in identities.values() if body not in (certificate,))
    return certificate, key


def reencoding_rejections(root: Path) -> None:
    scanner = root / 'tools/tls/scan.py'
    certificate, key = historical_identity(root)
    with tempfile.TemporaryDirectory(prefix='tls-reenc-') as directory:
        base = Path(directory)
        material = base / 'material'
        fixture = base / 'fixture'
        material.mkdir()
        fixture.mkdir()
        cert_pem = material / 'input.crt'
        key_pem = material / 'input.key'
        cert_pem.write_bytes(certificate)
        key_pem.write_bytes(key)
        trusted = subprocess.run(['openssl', 'x509', '-trustout', '-in', str(cert_pem)],
                                 capture_output=True, check=True).stdout
        cert_der = subprocess.run(['openssl', 'x509', '-outform', 'DER', '-in', str(cert_pem)],
                                  capture_output=True, check=True).stdout
        key_der = subprocess.run(['openssl', 'pkey', '-outform', 'DER', '-in', str(key_pem)],
                                 capture_output=True, check=True).stdout
        key_der_pkcs1 = subprocess.run(['openssl', 'rsa', '-outform', 'DER', '-in', str(key_pem)],
                                       capture_output=True, check=True).stdout
        denied_der = set(subprocess.run(
            [sys.executable, '-B', str(scanner), '--print-der-denylist'],
            capture_output=True, text=True, check=True).stdout.split())
        expected_der = {hashlib.sha256(body).hexdigest()
                        for body in (cert_der, key_der, key_der_pkcs1)}
        assert denied_der == expected_der, (denied_der, expected_der)
        cases = {
            'docs/retro-notes.md': trusted,
            'assets/level-map.bin': cert_der,
            'src/aux/lookup-table.inc': base64.b64encode(certificate),
            'include/aux/defaults.dat': base64.b64encode(key),
            'tests/data/sample.hexdump': cert_der.hex().encode(),
            'third_party/aasdk/docs/misc.txt': base64.b64encode(key_der_pkcs1),
            'config/local-cache.b64': base64.b64encode(cert_der),
            'notes/todo.txt': key_der.hex().encode(),
        }
        control = fixture / 'docs' / 'retro-notes.md'
        control.parent.mkdir(parents=True, exist_ok=True)
        control.write_text('benign planning notes; no credential material\n')
        clean = run(scanner, fixture)
        assert clean.returncode == 0, clean
        assert 'PASS' in clean.stdout, clean
        control.unlink()
        for name, payload in cases.items():
            target = fixture / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(payload)
            result = run(scanner, fixture)
            assert result.returncode != 0, (name, result)
            assert result.stdout == 'tls-key-scan: FAIL credential-material-present\n', (name, result)
            assert 'PASS' not in result.stdout and 'PASS' not in result.stderr, (name, result)
            target.unlink()


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    if '--negative' in sys.argv:
        plant_rejections(root)
        print('tls-key-scan-negative: PASS planted-marker-name-cert-rejected')
        return 0
    if '--reencodings' in sys.argv:
        reencoding_rejections(root)
        print('tls-key-scan-reencodings: PASS historical-reencodings-rejected-under-unrelated-names')
        return 0
    if '--invalid-root' in sys.argv:
        case = sys.argv[sys.argv.index('--invalid-root') + 1]
        invalid_root(root, case)
        print(f'tls-key-scan-invalid-root: PASS {case}-rejected')
        return 0
    prove_absence(root)
    print('tls-key-scan-absence: PASS zero-credential-copies denylist-bound-to-provenance-record')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
