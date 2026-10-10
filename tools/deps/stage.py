# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/deps/stage.py --archive ARCHIVE --cache build/deps
"""Materialize and verify immutable offline dependency sources in ignored build/."""
from __future__ import annotations

import argparse
from collections.abc import Mapping
from dataclasses import dataclass
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import sys
import tarfile
import tempfile

from check_manifest import PATCH_PATH, PATCH_SHA256, TLS_PATCH_SHA256, SOURCES, ManifestError, check
from tls_patch import apply as apply_tls


@dataclass(frozen=True, slots=True)
class File:
    data: bytes
    mode: int


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def blob(data: bytes) -> str:
    return hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()


def archive_files(data: bytes) -> Mapping[str, File]:
    """Extract regular files to memory; never follow links or archive paths."""
    files: dict[str, File] = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode='r:gz') as archive:
        for member in archive.getmembers():
            path = PurePosixPath(member.name)
            if path.is_absolute() or '..' in path.parts or path.parts[0] != 'googletest-1.15.2':
                raise ManifestError('UNSAFE_ARCHIVE', member.name)
            if member.isdir():
                continue
            if not member.isfile():
                raise ManifestError('UNSAFE_ARCHIVE', member.name)
            relative = Path('googletest', *path.parts[1:]).as_posix()
            if relative in files:
                raise ManifestError('UNSAFE_ARCHIVE', member.name)
            stream = archive.extractfile(member)
            if stream is None:
                raise ManifestError('UNSAFE_ARCHIVE', member.name)
            with stream:
                files[relative] = File(stream.read(), 0o755 if member.mode & 0o111 else 0o644)
    return files


def verify(stage: Path, expected: Mapping[str, File]) -> None:
    """Verify all staged bytes, modes and paths, including previously reused caches."""
    directories = {parent.as_posix() for name in expected
                   for parent in PurePosixPath(name).parents if parent.as_posix() != '.'}
    actual: set[str] = set()
    for path in stage.rglob('*'):
        relative = path.relative_to(stage).as_posix()
        if path.is_symlink():
            raise ManifestError('STAGE_MISMATCH', relative)
        if path.is_dir() and relative in directories:
            continue
        wanted = expected.get(relative)
        if wanted is None or not path.is_file():
            raise ManifestError('STAGE_MISMATCH', relative)
        if path.read_bytes() != wanted.data or stat.S_IMODE(path.stat().st_mode) != wanted.mode:
            raise ManifestError('STAGE_MISMATCH', relative)
        actual.add(relative)
    if actual != set(expected):
        raise ManifestError('STAGE_MISMATCH', 'missing-files')


def stage(root: Path, cache: Path, archive_path: Path) -> Path:
    """Anchor originals, apply the locked patch, and publish a content-addressed stage."""
    manifest = root / 'deps/manifest.json'
    lock = root / 'deps/manifest.lock'
    check(manifest, lock, None)
    data = archive_path.read_bytes()
    if sha256(data) != SOURCES[2].archive_sha256:
        raise ManifestError('ARCHIVE_MISMATCH', str(archive_path))
    # Invoke task 2 unchanged: its independent upstream listing/blob anchors win.
    provenance = subprocess.run(
        [sys.executable, '-B', str(root / 'tools/provenance/check.py'), '--root', str(root)],
        capture_output=True, text=True, check=False,
    )
    if provenance.returncode != 0:
        raise ManifestError('PROVENANCE_FAILED', provenance.stdout + provenance.stderr)
    if not cache.resolve().is_relative_to((root / 'build').resolve()):
        raise ManifestError('CACHE_LOCATION', str(cache))
    for path in (cache, *cache.parents):
        if path == root:
            break
        if path.is_symlink():
            raise ManifestError('CACHE_LOCATION', str(path))
    cache.mkdir(parents=True, exist_ok=True)
    scratch = Path(tempfile.mkdtemp(prefix='audit-', dir=cache))
    original = root / 'third_party/aasdk/CMakeLists.txt'
    original_data = original.read_bytes()
    if blob(original_data) != '1b1eb749423a7d017a6917cf7199ee583436d34c':
        raise ManifestError('ORIGINAL_BLOB_MISMATCH', str(original))
    patched = scratch / 'effective-CMakeLists.txt'
    result = subprocess.run(
        ['patch', '--batch', '--fuzz=0', '--posix', '--output', str(patched),
         '--input', str(root / 'deps' / PATCH_PATH), str(original)],
        capture_output=True, text=True, check=False,
    )
    if result.returncode != 0:
        raise ManifestError('PATCH_APPLICATION', result.stdout + result.stderr)
    effective = patched.read_bytes()
    block = re.search(rb'(?ms)^if\(AASDK_TEST\)\n.*?^endif\(AASDK_TEST\)\n', effective)
    if block is None:
        raise ManifestError('PATCH_APPLICATION', 'missing dependency block')
    tags = re.findall(rb'\bGIT_TAG\s+(\S+)', block.group())
    if tags != [SOURCES[2].commit.encode('ascii')]:
        raise ManifestError('PATCH_APPLICATION', 'unexpected GoogleTest identity')
    expected = dict(archive_files(data))
    for path in (root / 'third_party/aasdk').rglob('*'):
        if path.is_file():
            name = 'aasdk/' + path.relative_to(root / 'third_party/aasdk').as_posix()
            content = effective if name == 'aasdk/CMakeLists.txt' else path.read_bytes()
            expected[name] = File(content, stat.S_IMODE(path.stat().st_mode))
    expected['googletest-declaration.cmake'] = File(block.group(), 0o644)
    for patched_file in apply_tls(root, effective):
        name = 'aasdk/' + patched_file.path
        if patched_file.contents is None:
            del expected[name]
        else:
            expected[name] = File(patched_file.contents, expected[name].mode)
    effective = expected['aasdk/CMakeLists.txt'].data
    identity = {
        'schema': 'aa-effective-dependencies-1',
        'original_cmake_blob': blob(original_data), 'original_cmake_sha256': sha256(original_data),
        'patch_sha256': PATCH_SHA256,
        'tls_patch_sha256': TLS_PATCH_SHA256,
        'effective_cmake_blob': blob(effective), 'effective_cmake_sha256': sha256(effective),
        'googletest_archive_sha256': sha256(data),
        'effective_googletest_git_tag': tags[0].decode('ascii'),
        'source_files_sha256': sha256(json.dumps(
            {name: [sha256(file.data), file.mode] for name, file in sorted(expected.items())},
            sort_keys=True).encode()),
    }
    expected['identity.json'] = File((json.dumps(identity, indent=2) + '\n').encode(), 0o644)
    key = sha256(lock.read_bytes() + expected['identity.json'].data)
    destination = cache / key
    if destination.is_symlink():
        raise ManifestError('STAGE_MISMATCH', str(destination))
    if destination.exists():
        verify(destination, expected)
        return destination
    prepared = scratch / 'stage'
    prepared.mkdir()
    for name, file in expected.items():
        output = prepared / name
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(file.data)
        output.chmod(file.mode)
    prepared.rename(destination)
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = stage(args.root.resolve(), args.cache.absolute(), args.archive.resolve())
    except ManifestError as error:
        print(error, file=sys.stderr)
        return 1
    except (OSError, tarfile.TarError) as error:
        print(f'STAGING_IO_ERROR\t{error}', file=sys.stderr)
        return 1
    print(result)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
