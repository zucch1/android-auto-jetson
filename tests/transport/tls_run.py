# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/transport/tls_run.py ROOT BINARY
"""Run the task-16 TLS integration binary under a generated HU credential.

Provisions HU_KEY_PATH from a synthetic neutral-DN self-signed credential
(generated fresh per run by tools/tls/synthetic_credential.py; no third-party
credential material exists in the tree), runs the gtest binary (real TLS
handshake + encrypted round trip + unknown-peer), and checks that no
credential material leaks into the output.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main() -> int:
    root, binary = Path(sys.argv[1]), sys.argv[2]
    environment = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix='transport-tls-') as directory:
        work = Path(directory)
        subprocess.run([sys.executable, '-B', str(root / 'tools/tls/synthetic_credential.py'),
                        str(work / 'generated')], capture_output=True, text=True, check=True)
        generated = work / 'generated'
        bundle = work / 'sensitive-credential.pem'
        bundle.write_bytes((generated / 'synthetic-hu.crt').read_bytes() +
                           (generated / 'synthetic-hu.key').read_bytes())
        bundle.chmod(0o600)
        environment['HU_KEY_PATH'] = str(bundle)
        # Given: a generated synthetic credential in a secure bundle. When: run the suite.
        result = subprocess.run([binary], env=environment, capture_output=True,
                                text=True, timeout=60, check=False)
        combined = result.stdout + result.stderr
        # Then: the suite passes and no credential material is echoed.
        assert directory not in combined, combined
        assert 'sensitive-credential' not in combined, combined
        assert '-----BEGIN' not in combined, combined
        if result.returncode != 0:
            print(combined, end='')
            return result.returncode
        print('transport-tls-suite=passed redacted=yes')
        return 0


if __name__ == '__main__':
    raise SystemExit(main())
