# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/transport/tls_run.py ROOT BINARY
"""Run the task-16 TLS integration binary under a real HU credential (task 8).

Provisions HU_KEY_PATH from the pinned public reference, runs the gtest binary
(real TLS handshake + encrypted round trip + unknown-peer), and checks that no
credential material leaks into the output.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile


def main() -> int:
    root, binary = Path(sys.argv[1]), sys.argv[2]
    reference = root / 'third_party' / 'compat-credentials'
    environment = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix='transport-tls-') as directory:
        bundle = Path(directory) / 'sensitive-credential.pem'
        bundle.write_bytes((reference / 'headunit.crt').read_bytes() +
                           (reference / 'headunit.key').read_bytes())
        bundle.chmod(0o600)
        environment['HU_KEY_PATH'] = str(bundle)
        # Given: the pinned credential in a secure bundle. When: run the suite.
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
