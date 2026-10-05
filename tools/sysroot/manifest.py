# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Import tools.sysroot.manifest; CLI: python3 -B tools/sysroot/cli.py --help
"""Strict stdlib JSON boundary and canonical external manifest encoding."""
from __future__ import annotations

from collections.abc import Mapping
from dataclasses import asdict
import hashlib
import json
import re
from enum import StrEnum
from typing import assert_never

from .models import (Architecture, Code, Digest, Entry, Identity, InputPath, Manifest,
                     Package, PackageName, Provenance, RegularFile, RelativePath,
                     Symlink, SysrootError, Version)

type Json = str | int | float | bool | None | list[Json] | dict[str, Json]


class Kind(StrEnum):
    REGULAR = 'regular'
    SYMLINK = 'symlink'


def pairs(values: list[tuple[str, Json]]) -> dict[str, Json]:
    result: dict[str, Json] = {}
    for key, value in values:
        if key in result:
            raise SysrootError(Code.DUPLICATE, key)
        result[key] = value
    return result


def mapping(value: Json) -> Mapping[str, Json]:
    if not isinstance(value, dict):
        raise SysrootError(Code.MALFORMED_METADATA, 'object')
    return value


def sequence(value: Json) -> list[Json]:
    if not isinstance(value, list) or not value:
        raise SysrootError(Code.MALFORMED_METADATA, 'array')
    return value


def text(value: Json) -> str:
    if not isinstance(value, str) or not value.strip() or '\x00' in value:
        raise SysrootError(Code.MALFORMED_METADATA, 'text')
    return value


def optional_text(value: Json) -> str | None:
    return None if value is None else text(value)


def keys(record: Mapping[str, Json], expected: set[str]) -> None:
    if set(record) != expected:
        raise SysrootError(Code.MALFORMED_METADATA, 'keys')


def relative(value: Json) -> RelativePath:
    path = text(value)
    if path.startswith('/') or '\\' in path or any(p in ('', '.', '..') for p in path.split('/')):
        raise SysrootError(Code.UNSAFE_PATH, path)
    return RelativePath(path)


def package(value: Json) -> Package:
    record = mapping(value)
    keys(record, {'name', 'version', 'architecture', 'source', 'all_justification'})
    name = text(record['name'])
    version = text(record['version'])
    base_name, separator, qualifier = name.partition(':')
    if (re.fullmatch(r'[a-z0-9][a-z0-9+.-]*', base_name) is None
            or (separator and qualifier != text(record['architecture'])) or re.fullmatch(
            r'(?:[0-9]+:)?[0-9][0-9A-Za-z.+~]*(?:-[0-9A-Za-z.+~]+)*', version) is None):
        raise SysrootError(Code.MALFORMED_METADATA, 'package.identity')
    try:
        arch = Architecture(text(record['architecture']))
    except ValueError as error:
        raise SysrootError(Code.MALFORMED_METADATA, 'package.architecture') from error
    justification = optional_text(record['all_justification'])
    match arch:
        case Architecture.ARM64:
            if justification is not None:
                raise SysrootError(Code.MALFORMED_METADATA, 'all_justification')
        case Architecture.ALL:
            if justification is None:
                raise SysrootError(Code.MALFORMED_METADATA, 'all_justification')
        case unreachable:
            assert_never(unreachable)
    return Package(PackageName(name), version, arch, text(record['source']), justification)


def entry(value: Json) -> Entry:
    record = mapping(value)
    try:
        kind = Kind(text(record.get('kind')))
    except ValueError as error:
        raise SysrootError(Code.MALFORMED_METADATA, 'file.kind') from error
    expected = {'kind', 'original_input_path', 'staged_path', 'package', 'sha256'}
    match kind:
        case Kind.REGULAR:
            keys(record, expected)
        case Kind.SYMLINK:
            keys(record, expected | {'link_text'})
        case unreachable:
            assert_never(unreachable)
    original = text(record['original_input_path'])
    if not original.startswith('/'):
        raise SysrootError(Code.UNSAFE_PATH, original)
    relative(original[1:])
    staged = relative(record['staged_path'])
    sha = text(record['sha256'])
    if re.fullmatch('[0-9a-f]{64}', sha) is None:
        raise SysrootError(Code.MALFORMED_METADATA, 'sha256')
    owner = PackageName(text(record['package']))
    match kind:
        case Kind.REGULAR:
            return RegularFile(InputPath(original), staged, owner, Digest(sha))
        case Kind.SYMLINK:
            link = text(record['link_text'])
            if link.startswith('/') or '\\' in link:
                raise SysrootError(Code.SYMLINK_CLOSURE, staged)
            if hashlib.sha256(link.encode('utf-8')).hexdigest() != sha:
                raise SysrootError(Code.HASH_MISMATCH, staged)
            return Symlink(InputPath(original), staged, owner, Digest(sha), link)
        case unreachable:
            assert_never(unreachable)


def closure(entries: tuple[Entry, ...], staged: bool) -> tuple[InputPath, ...]:
    paths = {(e.staged_path if staged else e.original_input_path[1:]): e for e in entries}
    targets: list[InputPath] = []
    for path in paths:
        parts = path.split('/')
        if any('/'.join(parts[:i]) in paths for i in range(1, len(parts))):
            raise SysrootError(Code.UNSAFE_PATH, path)
        visited: set[str] = set()
        current = path
        while True:
            if current in visited or current not in paths:
                raise SysrootError(Code.SYMLINK_CLOSURE, path)
            visited.add(current)
            match paths[current]:
                case RegularFile(original_input_path=original):
                    targets.append(original)
                    break
                case Symlink(link_text=link):
                    parts = current.split('/')[:-1]
                    components = link.split('/')
                    for i, component in enumerate(components):
                        if component == '..':
                            if not parts:
                                raise SysrootError(Code.SYMLINK_CLOSURE, path)
                            parts.pop()
                        elif component not in ('', '.'):
                            parts.append(component)
                        if i < len(components) - 1 and '/'.join(parts) in paths:
                            raise SysrootError(Code.SYMLINK_CLOSURE, path)
                    current = '/'.join(parts)
                case unreachable:
                    assert_never(unreachable)
    return tuple(targets)


def parse_manifest(raw: bytes) -> Manifest:
    """Parse external JSON once; unknown fields and duplicate JSON keys fail closed."""
    try:
        value: Json = json.loads(raw, object_pairs_hook=pairs)
        record = mapping(value)
        keys(record, {'schema', 'version', 'provenance', 'identity', 'capability', 'packages', 'files'})
        if record['schema'] != 'aa-sysroot-1' or record['capability'] != 'pending':
            raise SysrootError(Code.MALFORMED_METADATA, 'schema/capability')
        version = text(record['version'])
        if re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._-]{0,127}', version) is None:
            raise SysrootError(Code.UNSAFE_PATH, 'version')
        provenance = Provenance(text(record['provenance']))
        identity = mapping(record['identity'])
        keys(identity, {'kernel', 'l4t'})
        packages = tuple(package(p) for p in sequence(record['packages']))
        files = tuple(entry(f) for f in sequence(record['files']))
        names = {p.name for p in packages}
        if len(names) != len(packages) or len({f.staged_path for f in files}) != len(files) or len(
                {f.original_input_path for f in files}) != len(files):
            raise SysrootError(Code.DUPLICATE, 'inventory')
        if any(f.package not in names for f in files):
            raise SysrootError(Code.PACKAGE_OWNER, 'files')
        if closure(files, staged=False) != closure(files, staged=True):
            raise SysrootError(Code.SYMLINK_CLOSURE, 'relocated referent')
        return Manifest(Version(version), provenance,
                        Identity(optional_text(identity['kernel']), optional_text(identity['l4t'])),
                        tuple(sorted(packages, key=lambda p: p.name)),
                        tuple(sorted(files, key=lambda f: f.staged_path)))
    except (ValueError, UnicodeDecodeError, RecursionError) as error:
        raise SysrootError(Code.MALFORMED_METADATA, 'json') from error


def encode_manifest(manifest: Manifest) -> bytes:
    files = []
    for file in manifest.files:
        record = asdict(file)
        match file:
            case RegularFile():
                record['kind'] = 'regular'
            case Symlink():
                record['kind'] = 'symlink'
            case unreachable:
                assert_never(unreachable)
        files.append(record)
    return (json.dumps({'schema': 'aa-sysroot-1', 'version': manifest.version,
        'provenance': manifest.provenance, 'identity': asdict(manifest.identity),
        'capability': 'pending', 'packages': [asdict(p) for p in manifest.packages],
        'files': files}, sort_keys=True, separators=(',', ':')) + '\n').encode('utf-8')


def manifest_digest(manifest: Manifest) -> Digest:
    return Digest(hashlib.sha256(encode_manifest(manifest)).hexdigest())
