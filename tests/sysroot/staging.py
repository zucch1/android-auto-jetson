# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tests/sysroot/staging.py
"""Real local CLI behavior; fixtures isolated by standard temporary contexts."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from fixtures import create, expect


def state(stage: Path) -> tuple[tuple[str, int, int, str], ...]:
    if not stage.exists():
        return ()
    result = []
    for path in (stage, *sorted(stage.rglob('*'))):
        info = path.lstat()
        content = str(path.readlink()) if path.is_symlink() else (
            hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else '')
        result.append((str(path), info.st_mode, info.st_mtime_ns, content))
    return tuple(result)


def scenario(case: str, base: Path) -> None:
    # Given: only test code manufactures synthetic fixture bytes.
    root = Path(__file__).resolve().parents[2]
    fixture = create(root, base)
    doc = fixture.document()
    data = fixture.snapshot / 'usr/share/data'
    stage = fixture.destination / 'v1'
    expected = ''
    copied = b'synthetic bytes\n'
    if case in ('reuse', 'new-version', 'package-drift', 'content-drift', 'tamper',
                'stage-extra', 'stage-missing', 'stage-mode', 'stage-link', 'guard'):
        assert fixture.run().returncode == 0
    before = state(stage)
    match case:
        case 'happy' | 'reuse' | 'wrapper':
            pass
        case 'new-version':
            copied = b'new-version synthetic bytes'
            data.write_bytes(copied)
            doc = fixture.document(version='v2', content=copied)
        case 'package-drift':
            packages = doc['packages']
            assert isinstance(packages, list) and isinstance(packages[0], dict)
            packages[0]['version'] = '2.0-1'
            expected = 'NEW_VERSION_REQUIRED'
        case 'content-drift':
            data.write_bytes(b'changed synthetic bytes')
            doc = fixture.document(content=b'changed synthetic bytes')
            expected = 'NEW_VERSION_REQUIRED'
        case 'tamper':
            (stage / 'rootfs/usr/share/data').chmod(0o644)
            (stage / 'rootfs/usr/share/data').write_bytes(b'tamper')
            expected = 'ARTIFACT_MISMATCH'
        case 'stage-extra':
            (stage / 'rootfs/usr/share').chmod(0o755)
            (stage / 'rootfs/usr/share/extra').write_bytes(b'extra')
            expected = 'ARTIFACT_MISMATCH'
        case 'stage-missing':
            (stage / 'rootfs/usr/share').chmod(0o755)
            (stage / 'rootfs/usr/share/data').unlink()
            expected = 'ARTIFACT_MISMATCH'
        case 'stage-mode':
            (stage / 'rootfs/usr/share/data').chmod(0o644)
            expected = 'ARTIFACT_MISMATCH'
        case 'stage-link':
            (stage / 'rootfs/usr/share').chmod(0o755)
            (stage / 'rootfs/usr/share/alias').unlink()
            (stage / 'rootfs/usr/share/alias').symlink_to('/etc/passwd')
            expected = 'ARTIFACT_MISMATCH'
        case 'missing':
            data.unlink()
            expected = 'INVENTORY_MISMATCH'
        case 'extra':
            data.with_name('extra').write_bytes(b'extra')
            expected = 'INVENTORY_MISMATCH'
        case 'wrong-hash':
            data.write_bytes(b'changed')
            expected = 'HASH_MISMATCH'
        case 'duplicate-package':
            packages = doc['packages']
            assert isinstance(packages, list)
            packages.append(packages[0])
            expected = 'DUPLICATE'
        case 'duplicate-file':
            files = doc['files']
            assert isinstance(files, list)
            files.append(files[0])
            expected = 'DUPLICATE'
        case 'traversal' | 'absolute-path' | 'unknown-owner' | 'unlisted-input':
            files = doc['files']
            assert isinstance(files, list) and isinstance(files[0], dict)
            field, value, expected = {
                'traversal': ('staged_path', '../escape', 'UNSAFE_PATH'),
                'absolute-path': ('staged_path', '/usr/data', 'UNSAFE_PATH'),
                'unknown-owner': ('package', 'unknown', 'PACKAGE_OWNER'),
                'unlisted-input': ('original_input_path', '/usr/share/other', 'INVENTORY_MISMATCH'),
            }[case]
            selected = files[1] if case == 'unlisted-input' else files[0]
            assert isinstance(selected, dict)
            selected[field] = value
        case 'symlink-escape':
            alias = fixture.snapshot / 'usr/share/alias'
            alias.unlink()
            alias.symlink_to('../../../outside')
            files = doc['files']
            assert isinstance(files, list) and isinstance(files[1], dict)
            files[1]['link_text'] = '../../../outside'
            files[1]['sha256'] = hashlib.sha256(b'../../../outside').hexdigest()
            expected = 'SYMLINK_CLOSURE'
        case 'snapshot-parent-link':
            link = base / 'snapshot-link'
            link.symlink_to(fixture.snapshot, target_is_directory=True)
            result = subprocess.run([sys.executable, '-B', str(root / 'tools/sysroot/cli.py'),
                'materialize', '--snapshot', str(link / 'usr'), '--metadata', str(fixture.metadata),
                '--destination', str(fixture.destination)], capture_output=True, text=True, check=False)
            expect(result, 'UNSAFE_PATH')
            return
        case 'directory-link':
            (fixture.snapshot / 'escape').symlink_to(base, target_is_directory=True)
            expected = 'INVENTORY_MISMATCH'
        case 'malformed':
            fixture.metadata.write_text('{')
            expect(fixture.run(), 'MALFORMED_METADATA')
            return
        case 'guard':
            expect(fixture.run('require-observed'), 'FIXTURE_PROVENANCE')
            return
        case _:
            raise AssertionError(case)
    fixture.write(doc)
    preserved = state(stage)
    # When: invoke the product, not a mock of its copying implementation.
    result = fixture.run('wrapper' if case == 'wrapper' else 'materialize')
    # Then: stable typed code or observable immutable publication.
    if expected:
        expect(result, expected)
        assert state(stage) == preserved
    else:
        assert result.returncode == 0, result
        artifact = Path(json.loads(result.stdout)['artifact'])
        assert (artifact / 'rootfs/usr/share/data').read_bytes() == copied
        assert (artifact / 'rootfs/usr/share/alias').readlink() == Path('data')
        assert fixture.run('validate').returncode == 0
        if case == 'new-version':
            validated = subprocess.run(
                [sys.executable, '-B', str(root / 'tools/sysroot/cli.py'), 'validate', '--artifact', str(artifact)],
                capture_output=True, text=True, check=False)
            assert validated.returncode == 0, validated
            assert Path(json.loads(validated.stdout)['artifact']) == artifact
        if before:
            assert before == state(stage)


def main() -> int:
    cases = ('happy', 'wrapper', 'reuse', 'new-version', 'package-drift', 'content-drift', 'tamper',
             'stage-extra', 'stage-missing', 'stage-mode', 'stage-link', 'missing', 'extra',
             'wrong-hash', 'duplicate-package', 'duplicate-file', 'traversal', 'absolute-path',
             'unknown-owner', 'unlisted-input', 'symlink-escape', 'snapshot-parent-link',
             'directory-link', 'malformed', 'guard')
    for case in cases:
        with tempfile.TemporaryDirectory(prefix=f'sysroot-{case}-') as temporary:
            scenario(case, Path(temporary))
        print(f'PASS {case}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
