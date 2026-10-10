# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/scan.py ROOT [--negative]
"""Assert ZERO credential copies remain and that the scanner still detects plants.

Positive mode runs the shipped no-allowlist marker/digest/name scan against the
real source tree (it must prove absence), and binds the scanner digest denylist
to the sanitization record in third_party/provenance/inventory.json. Negative
mode plants credential-shaped material in a private fixture and requires
rejection: private-key PEM blocks, certificate PEM blocks, the historical
credential file names and planted copies under source-like directories.
"""
from pathlib import Path
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


def denied_digests(scanner: Path) -> set[str]:
    result = subprocess.run([sys.executable, '-B', str(scanner), '--print-denylist'],
                            capture_output=True, text=True, check=True)
    return set(result.stdout.split())


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    if '--negative' in sys.argv:
        plant_rejections(root)
        print('tls-key-scan-negative: PASS planted-marker-name-cert-rejected')
        return 0
    prove_absence(root)
    print('tls-key-scan-absence: PASS zero-credential-copies denylist-bound-to-provenance-record')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
