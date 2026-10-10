# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tools/build/cache_publish.py publish --artifact <frozen-sysroot> --cache-root .local/build-cache
# python3 -B tools/build/cache_publish.py verify  --cache-root .local/build-cache --manifest-sha256 HEX
"""Publish a frozen sysroot as a content-addressed archive bound to its manifest digest.

The immutable private CI cache alternative to a container image: a deterministic tar of the
validated frozen artifact, addressed by its own sha256 and bound to the manifest digest. Reuse
re-validates the archive digest (rejecting tampering without repair) and rejects same-version drift.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tarfile
from enum import StrEnum
from typing import assert_never

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.manifest import manifest_digest as compute_digest, parse_manifest
from tools.sysroot.models import (Code, Digest, Manifest, Provenance, SysrootError, Version,
                                  require_observed)
from tools.sysroot.stage import validate_artifact

RECORD_KEYS = frozenset({'manifest_digest', 'archive_sha256', 'archive',
                         'version', 'provenance', 'acceptance'})
VERSION_RE = re.compile(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}')
DIGEST_RE = re.compile('[0-9a-f]{64}')


@dataclass(frozen=True, slots=True)
class Publication:
    manifest_digest: Digest
    archive_sha256: Digest
    archive: Path
    version: Version
    provenance: Provenance
    acceptance: str
    archive_reused: bool
    current_rebound: bool


@dataclass(frozen=True, slots=True)
class Verification:
    manifest_digest: Digest
    archive_sha256: Digest
    archive: Path
    acceptance: str


class Command(StrEnum):
    PUBLISH = 'publish'
    VERIFY = 'verify'


def acceptance(provenance: Provenance) -> str:
    return 'target-observed' if provenance is Provenance.OBSERVED else 'fixture-not-target'


def _walk_sorted(root: Path) -> list[Path]:
    """Literal entries (dirs, regular, symlinks) relative to root, sorted; never follows symlinks."""
    entries: list[Path] = []
    stack: list[Path] = [Path('.')]
    while stack:
        base = root / stack.pop()
        for item in sorted(base.iterdir()):
            relative = item.relative_to(root)
            entries.append(relative)
            if not item.is_symlink() and item.is_dir():
                stack.append(relative)
    return sorted(entries)


def _write_archive(objects: Path, artifact: Path) -> tuple[Digest, Path, bool]:
    """Stream a deterministic tar of the frozen artifact into the content-addressed store."""
    objects.mkdir(parents=True, exist_ok=True)
    temporary = objects / f'.stage-{os.getpid()}.tar'
    with temporary.open('wb') as raw, tarfile.open(fileobj=raw, mode='w',
                                                  format=tarfile.GNU_FORMAT) as tar:
        for relative in _walk_sorted(artifact):
            source = artifact / relative
            info = tar.gettarinfo(str(source), arcname=relative.as_posix())
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            info.mtime = 0
            info.mode = 0o555 if info.isdir() else 0o777 if info.issym() else 0o444
            if info.isreg():
                with source.open('rb') as stream:
                    tar.addfile(info, stream)
            else:
                tar.addfile(info)
    with temporary.open('rb') as stream:
        archive_sha256 = Digest(hashlib.file_digest(stream, 'sha256').hexdigest())
    archive = objects / f'{archive_sha256}.tar'
    if archive.exists():
        with archive.open('rb') as stream:
            if hashlib.file_digest(stream, 'sha256').hexdigest() != archive_sha256:
                temporary.unlink()
                raise SysrootError(Code.HASH_MISMATCH, str(archive))
        temporary.unlink()
        return archive_sha256, archive, True
    os.rename(temporary, archive)
    archive.chmod(0o444)
    return archive_sha256, archive, False


def _write_once(path: Path, record: dict[str, str]) -> bool:
    if path.exists():
        if json.loads(path.read_text()) != record:
            raise SysrootError(Code.NEW_VERSION_REQUIRED, str(path))
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, sort_keys=True))
    path.chmod(0o444)
    return False


def publish(artifact: Path, cache_root: Path, observed_only: bool) -> Publication:
    """Create or reuse the content-addressed archive and its manifest-digest binding."""
    staged = validate_artifact(artifact)
    manifest, digest = staged.manifest, staged.manifest_sha256
    if observed_only:
        require_observed(manifest)
    current = cache_root / 'current.json'
    rebound = False
    previous: dict[str, str] | None = None
    if current.exists():
        previous = _read_record(current, set(RECORD_KEYS))
        if previous['manifest_digest'] == digest:
            rebound = False
        elif previous['version'] == manifest.version:
            raise SysrootError(Code.NEW_VERSION_REQUIRED, 'current.same_version')
        else:
            prior = cache_root / 'bindings' / f"{previous['manifest_digest']}.json"
            if not prior.exists() or _read_record(prior, set(RECORD_KEYS)) != previous:
                raise SysrootError(Code.ARTIFACT_MISMATCH, str(current))
            rebound = True
    archive_sha256, archive, archive_reused = _write_archive(cache_root / 'objects', artifact)
    record = {'manifest_digest': digest, 'archive_sha256': archive_sha256,
              'archive': archive.name, 'version': manifest.version,
              'provenance': manifest.provenance, 'acceptance': acceptance(manifest.provenance)}
    if previous is not None and previous['manifest_digest'] == digest and previous != record:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(current))
    binding_reused = _write_once(cache_root / 'bindings' / f'{digest}.json', record)
    if rebound or previous is None:
        current.write_text(json.dumps(record, sort_keys=True))
    return Publication(digest, archive_sha256, archive, manifest.version, manifest.provenance,
                       acceptance(manifest.provenance), archive_reused or binding_reused, rebound)


def _read_record(path: Path, expected_keys: set[str]) -> dict[str, str]:
    try:
        record = json.loads(path.read_text())
    except json.JSONDecodeError as error:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(path)) from error
    if not isinstance(record, dict) or set(record) != expected_keys or not all(
            isinstance(value, str) for value in record.values()):
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(path))
    for field in ('manifest_digest', 'archive_sha256'):
        if field in record and DIGEST_RE.fullmatch(record[field]) is None:
            raise SysrootError(Code.ARTIFACT_MISMATCH, str(path))
    if 'version' in record and VERSION_RE.fullmatch(record['version']) is None:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(path))
    if 'provenance' in record:
        try:
            provenance = Provenance(record['provenance'])
        except ValueError as error:
            raise SysrootError(Code.ARTIFACT_MISMATCH, str(path)) from error
        if record.get('acceptance') != acceptance(provenance):
            raise SysrootError(Code.ARTIFACT_MISMATCH, str(path))
    return record


def _archive_manifest(archive: Path) -> Manifest:
    try:
        with tarfile.open(archive, 'r:') as tar:
            member = tar.getmember('manifest.json')
            if not member.isreg():
                raise SysrootError(Code.ARTIFACT_MISMATCH, str(archive))
            extracted = tar.extractfile(member)
            assert extracted is not None
            return parse_manifest(extracted.read())
    except (tarfile.TarError, KeyError, SysrootError) as error:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(archive)) from error


def verify(cache_root: Path, manifest_digest: Digest, observed_only: bool) -> Verification:
    """Re-validate an existing archive digest read-only; reject tampering without repair."""
    binding = cache_root / 'bindings' / f'{manifest_digest}.json'
    if not binding.exists():
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(binding))
    record = _read_record(binding, {'manifest_digest', 'archive_sha256', 'archive',
                                    'version', 'provenance', 'acceptance'})
    if record['manifest_digest'] != manifest_digest:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(binding))
    if record['archive'] != f"{record['archive_sha256']}.tar":
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(binding))
    archive = cache_root / 'objects' / f'{record["archive_sha256"]}.tar'
    if not archive.exists():
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(archive))
    with archive.open('rb') as stream:
        if hashlib.file_digest(stream, 'sha256').hexdigest() != record['archive_sha256']:
            raise SysrootError(Code.HASH_MISMATCH, str(archive))
    manifest = _archive_manifest(archive)
    if compute_digest(manifest) != manifest_digest:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(archive))
    if observed_only:
        require_observed(manifest)
    if record['version'] != manifest.version or record['provenance'] != manifest.provenance:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(binding))
    if record['acceptance'] != acceptance(manifest.provenance):
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(binding))
    return Verification(manifest_digest, Digest(record['archive_sha256']), archive,
                        record['acceptance'])


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    publish_command = commands.add_parser('publish', help='Publish frozen sysroot to the cache')
    publish_command.add_argument('--artifact', type=Path, required=True)
    publish_command.add_argument('--cache-root', type=Path, required=True)
    publish_command.add_argument('--require-observed', action='store_true')
    verify_command = commands.add_parser('verify', help='Validate an existing cache archive digest')
    verify_command.add_argument('--cache-root', type=Path, required=True)
    verify_command.add_argument('--manifest-sha256', required=True)
    verify_command.add_argument('--require-observed', action='store_true')
    return parser.parse_args()


def main() -> int:
    args = arguments()
    try:
        match Command(args.command):
            case Command.PUBLISH:
                assert args.artifact is not None and args.cache_root is not None
                result = publish(args.artifact, args.cache_root, args.require_observed)
                output = {'manifest_digest': result.manifest_digest,
                          'archive_sha256': result.archive_sha256, 'archive': str(result.archive),
                          'acceptance': result.acceptance, 'archive_reused': result.archive_reused,
                          'current_rebound': result.current_rebound}
            case Command.VERIFY:
                assert args.cache_root is not None and args.manifest_sha256
                result = verify(args.cache_root, Digest(args.manifest_sha256), args.require_observed)
                output = {'manifest_digest': result.manifest_digest,
                          'archive_sha256': result.archive_sha256, 'archive': str(result.archive),
                          'acceptance': result.acceptance}
            case unreachable:
                assert_never(unreachable)
    except SysrootError as error:
        print(json.dumps({'code': error.code, 'field': error.field}), file=sys.stderr)
        return 1
    except OSError as error:
        print(json.dumps({'code': Code.LOCAL_IO, 'errno': error.errno}), file=sys.stderr)
        return 1
    print(json.dumps(output))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
