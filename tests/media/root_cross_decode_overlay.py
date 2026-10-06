# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/media/root_cross_decode_overlay.py LOG_ROOT ARCHIVE
"""Run the root cross regression outside source/build and retain its QA logs."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    logs, archive = (Path(value).resolve() for value in sys.argv[1:])
    logs.mkdir(parents=True, exist_ok=True)
    retained = Path(tempfile.mkdtemp(prefix='run-', dir=logs))
    # Given an external fixture root, never beneath source or the retained log root.
    with tempfile.TemporaryDirectory(prefix='decode-root-cross-') as external:
        scratch = Path(external) / 'fixture'
        # When building and checking the real root project (without running ARM64).
        with (retained / 'driver.log').open('w') as stream:
            result = subprocess.run(
                [sys.executable, '-B', str(root / 'tests/media/cross_decode_overlay.py'),
                 '--root-project', '--archive', str(archive), '--scratch', str(scratch),
                 '--build', str(retained / 'build')],
                stdout=stream, stderr=subprocess.STDOUT, check=False, timeout=240)
        for path in scratch.glob('*.log'):
            shutil.copyfile(path, retained / path.name)
        receipt = scratch / 'receipt.json'
        if receipt.is_file():
            shutil.copyfile(receipt, retained / receipt.name)
        # Then propagate any failed cross check, retaining the underlying diagnostics.
        print((retained / 'driver.log').read_text())
        print(f'root cross logs: {retained}')
        return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
