# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/handshake.py ROOT BINARY CASE
"""Check real Cryptor process exits, TLS mode labeling and redacted denials."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
from typing import Final

EXPECTED: Final = {
    'approved': '', 'compat-selfsigned': '',
    'unknown': 'tls-phone-not-approved', 'absent': 'tls-phone-not-approved',
    'verified-unknown': 'tls-peer-verification-rejected',
    'verified-selfsigned': 'tls-peer-verification-rejected',
    'revoked': 'tls-phone-not-approved', 'inactive': 'tls-session-inactive',
    'deinit': 'tls-session-inactive',
}


def main() -> int:
    root, binary, case = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    # Given: only the pinned public reference, explicitly copied to a secure bundle.
    reference = root / 'third_party/compat-credentials'
    environment = dict(os.environ)
    with tempfile.TemporaryDirectory(prefix='tls-handshake-') as directory:
        bundle = Path(directory) / 'sensitive-credential.pem'
        bundle.write_bytes((reference / 'headunit.crt').read_bytes() + (reference / 'headunit.key').read_bytes())
        bundle.chmod(0o600)
        environment['HU_KEY_PATH'] = str(bundle)
        # When: drive the production Cryptor against a memory-BIO server.
        result = subprocess.run([binary, case], env=environment, capture_output=True,
                                text=True, timeout=15, check=False)
        # Then: true handshakes succeed; trust failures remain nonzero and redacted.
        reason = EXPECTED[case]
        assert result.returncode == (1 if reason else 0), result
        combined = result.stdout + result.stderr
        if reason:
            assert reason in combined, result
        else:
            assert 'memory-bio-handshake=success' in combined, result
            assert 'receiver-credential-presented=yes encrypted-payload=yes' in combined, result
        if case not in ('unknown', 'absent'):
            mode = 'verified-peer-test-only' if case.startswith('verified-') else 'encryption-only-compatibility'
            assert f'tls-mode={mode}' in combined, result
        assert directory not in combined and 'sensitive-credential' not in combined, result
        assert '-----BEGIN' not in combined, result
        print(f'handshake-case={case} binary_exit={result.returncode} redacted=yes')
        print(combined, end='')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
