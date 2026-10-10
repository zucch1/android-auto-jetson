# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Used by tools/deps/stage.py; never edits pristine sources.
"""Apply the digest-locked TLS text-edit recipe to in-memory effective sources."""
from pathlib import Path, PurePosixPath
from dataclasses import dataclass
import hashlib

from check_manifest import ManifestError, TLS_PATCH_PATH, load, mapping, text


@dataclass(frozen=True, slots=True)
class EffectiveFile:
    path: str
    contents: bytes | None


def apply(root: Path, effective_cmake: bytes) -> tuple[EffectiveFile, ...]:
    document = load(root / 'deps' / TLS_PATCH_PATH)
    if document.get('schema') != 'aa-locked-source-patch/1':
        raise ManifestError('TLS_PATCH_APPLICATION', 'schema')
    records = document.get('files')
    if not isinstance(records, list):
        raise ManifestError('TLS_PATCH_APPLICATION', 'files')
    results: list[EffectiveFile] = []
    names: set[str] = set()
    for raw in records:
        record = mapping(raw, 'tls-patch.file')
        name = text(record.get('path'), 'tls-patch.path')
        path = PurePosixPath(name)
        if path.is_absolute() or '..' in path.parts or name in names:
            raise ManifestError('TLS_PATCH_APPLICATION', 'path')
        names.add(name)
        original = (root / 'third_party/aasdk' / name).read_bytes()
        if hashlib.sha256(original).hexdigest() != record.get('original_sha256'):
            raise ManifestError('TLS_PATCH_APPLICATION', 'original-digest')
        if record.get('delete') is True:
            results.append(EffectiveFile(name, None))
            continue
        data = (effective_cmake if name == 'CMakeLists.txt' else original).decode()
        contents = record.get('contents')
        if contents is not None:
            data = text(contents, 'tls-patch.contents')
        removal = record.get('remove_between')
        if removal is not None:
            segment = mapping(removal, 'tls-patch.remove_between')
            start = text(segment.get('start'), 'tls-patch.start')
            end = text(segment.get('end'), 'tls-patch.end')
            if data.count(start) != 1 or data.count(end) != 1:
                raise ManifestError('TLS_PATCH_APPLICATION', 'ambiguous-removal')
            first, last = data.index(start), data.index(end)
            if last <= first:
                raise ManifestError('TLS_PATCH_APPLICATION', 'reversed-removal')
            data = data[:first] + data[last:]
        replacements = record.get('replace', [])
        if not isinstance(replacements, list):
            raise ManifestError('TLS_PATCH_APPLICATION', 'replace')
        for raw_replacement in replacements:
            replacement = mapping(raw_replacement, 'tls-patch.replacement')
            old = text(replacement.get('old'), 'tls-patch.old')
            new = text(replacement.get('new'), 'tls-patch.new')
            if data.count(old) != 1:
                raise ManifestError('TLS_PATCH_APPLICATION', 'ambiguous-replacement')
            data = data.replace(old, new)
        results.append(EffectiveFile(name, data.encode()))
    return tuple(results)
