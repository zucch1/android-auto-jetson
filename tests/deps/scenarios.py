# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/deps/scenarios.py ROOT CASE ARCHIVE
"""Given isolated dependency inputs, require observable CLI tamper rejection."""
from pathlib import Path
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    case = sys.argv[2]
    archive = Path(sys.argv[3]).resolve()
    # Given: private fixtures are retained, never shared or deleted.
    fixture = Path(tempfile.mkdtemp(prefix=f'deps-{case}-'))
    shutil.copytree(root / 'deps', fixture / 'deps')
    manifest = fixture / 'deps/manifest.json'
    lock = fixture / 'deps/manifest.lock'
    data = json.loads(manifest.read_text())
    expected = ''
    # When: mutate one input, then drive the public checker.
    match case:
        case 'valid':
            pass
        case 'missing-source':
            del data['sources']['aasdk']
            expected = 'SOURCE_SET'
        case 'source-sha':
            data['sources']['aasdk']['commit'] = '0' * 40
            expected = 'SOURCE_IDENTITY'
        case 'malformed-sha':
            data['sources']['googletest']['commit'] = 'v1.15.2'
            expected = 'MALFORMED_VALUE'
        case 'lock-digest':
            record = json.loads(lock.read_text())
            record['manifest_sha256'] = '0' * 64
            lock.write_text(json.dumps(record))
            expected = 'LOCK_MISMATCH'
        case 'version':
            data['host_packages'][0]['installed_version'] = 'latest'
            expected = 'MALFORMED_VERSION'
        case 'missing-package':
            data['host_packages'].pop(0)
            expected = 'PACKAGE_SET'
        case 'archive':
            altered = fixture / 'tampered.tar.gz'
            shutil.copyfile(archive, altered)
            with altered.open('ab') as stream:
                stream.write(b'tamper')
            archive = altered
            expected = 'ARCHIVE_MISMATCH'
        case 'patch':
            with (fixture / 'deps/patches/aasdk-googletest.patch').open('ab') as stream:
                stream.write(b'tamper')
            expected = 'PATCH_MISMATCH'
        case 'manifest-digest':
            with manifest.open('ab') as stream:
                stream.write(b' ')
            expected = 'LOCK_MISMATCH'
        case _:
            raise AssertionError(f'Unknown scenario: {case}')
    if case in ('missing-source', 'source-sha', 'malformed-sha', 'version', 'missing-package'):
        manifest.write_text(json.dumps(data))
        # Rebind only the fixture lock: schema/pin validation must still reject it.
        record = json.loads(lock.read_text())
        record['manifest_sha256'] = hashlib.sha256(manifest.read_bytes()).hexdigest()
        lock.write_text(json.dumps(record))
    result = subprocess.run(
        [sys.executable, '-B', str(root / 'tools/deps/check_manifest.py'),
         '--manifest', str(manifest), '--lock', str(lock), '--archive', str(archive)],
        capture_output=True, text=True, check=False,
    )
    # Then: stable error category and process exit, including a real happy path.
    print(f'case={case} checker_exit={result.returncode} fixture={fixture}')
    print(result.stdout + result.stderr, end='')
    assert result.returncode == (1 if expected else 0), result
    assert not expected or expected in result.stdout + result.stderr, result
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
