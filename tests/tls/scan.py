# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/scan.py ROOT [--negative]
"""Exercise the shipped marker scanner against isolated planted-key fixtures."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    scanner = root / 'tools/tls/scan.py'
    if '--negative' not in sys.argv:
        return subprocess.run([sys.executable, '-B', str(scanner), str(root)], check=False).returncode
    # Given: a private source fixture outside any real worktree.
    with tempfile.TemporaryDirectory(prefix='tls-scan-') as directory:
        fixture = Path(directory)
        shutil.copytree(root / 'third_party', fixture / 'third_party')
        for name in ('planted.pem', 'third_party/compat-credentials/injected.key',
                     'third_party/aasdk/src/Messenger/Cryptor.cpp'):
            target = fixture / name
            original = target.read_bytes() if target.exists() else None
            target.write_bytes((root / 'third_party/compat-credentials/headunit.key').read_bytes())
            # When: scan a planted key or a changed pinned exception.
            result = subprocess.run([sys.executable, '-B', str(scanner), str(fixture)],
                                    capture_output=True, text=True, check=False)
            # Then: exact path AND digest are required, not a directory exemption.
            assert result.returncode == 1, result
            assert result.stdout == 'tls-key-scan: FAIL forbidden-private-key-marker\n', result
            if original is None:
                target.unlink()
            else:
                target.write_bytes(original)
    print('tls-key-scan-negative: PASS planted-and-digest-tamper-rejected')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
