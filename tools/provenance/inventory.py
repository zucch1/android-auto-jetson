# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/provenance/inventory.py ROOT > inventory.json
"""Derive an audit inventory from anchored upstream listings and retained blobs."""
from collections import Counter
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import Final

type JsonValue = str | int | list[JsonValue] | dict[str, JsonValue]


@dataclass(frozen=True, slots=True)
class Source:
    """Immutable source identity and its audited distribution selection."""
    name: str
    url: str
    pin: str
    tree: str
    destination: str
    listing_sha256: str

    def includes(self, path: str) -> bool:
        return self.name == 'aasdk' or path in ('LICENSE', 'README.md') or (
            path.startswith('oaa/') and path.endswith('.proto'))


SOURCES: Final = (
    Source('aasdk', 'https://github.com/opencardev/aasdk.git',
           '9bf6adf933665dee26532201719fac14a047ccf1',
           '19fbb172a1f7390e4f6618018029de8b7f10c3b0', 'third_party/aasdk',
           '9327631eeae914a72eee8aa59aa639d07ec655b7b2e1fd90a4aafd367fd4521a'),
    Source('oaa', 'https://github.com/mrmees/open-android-auto.git',
           '61eab61c5f9968154ff1a80faa8c0a427b208479',
           '332f633e14dc23bbe4227eb3bde6242d0878fb00',
           'third_party/reference/open-android-auto/61eab61c5f9968154ff1a80faa8c0a427b208479',
           'e5bbc9a26bf5350cdfa03e09edb831cd53fc1c78f4cd7a4030ecc5e5067f3730'),
)
NOTICE: Final = re.compile(r'copyright|spdx|general public license|licensed under|'
                          r'all rights reserved|redistribution and use|permission is hereby granted|'
                          r'without warranty|licence', re.IGNORECASE)


@dataclass(frozen=True, slots=True)
class Entry:
    mode: str
    oid: str
    size: int
    path: str
    kind: str


@dataclass(frozen=True, slots=True)
class ProvenanceError(Exception):
    code: str
    path: str

    def __str__(self) -> str:
        return f'{self.code}\t{self.path}'


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def blob_oid(data: bytes) -> str:
    return hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()


def entries(root: Path, source: Source) -> tuple[Entry, ...]:
    """Parse only the exact pinned Git listing; reject substituted metadata."""
    path = root / 'third_party/provenance' / f'{source.name}.ls-tree'
    if path.is_symlink():
        raise ProvenanceError('SYMLINK_REJECTED', str(path))
    data = path.read_bytes()
    if sha256(data) != source.listing_sha256:
        raise ProvenanceError('UPSTREAM_LISTING_MISMATCH', str(path))
    result: list[Entry] = []
    for line in data.decode('utf-8').splitlines():
        meta, relative = line.split('\t', 1)
        mode, kind, oid, size = meta.split()
        regular = kind == 'blob' and mode in ('100644', '100755')
        excluded_link = kind == 'blob' and mode == '120000' and not source.includes(relative)
        if not (regular or excluded_link):
            raise ProvenanceError('UNSUPPORTED_UPSTREAM_OBJECT', relative)
        result.append(Entry(mode, oid, int(size), relative, kind))
    return tuple(result)


def render(root: Path) -> str:
    """Produce deterministic derived records; upstream listings remain authority."""
    components: list[JsonValue] = []
    copyright_lines: Counter[str] = Counter()
    for source in SOURCES:
        files: list[JsonValue] = []
        excluded: list[JsonValue] = []
        for entry in entries(root, source):
            record: dict[str, JsonValue] = {
                'path': entry.path, 'mode': entry.mode, 'size': entry.size,
                'original_git_object_oid': entry.oid,
                'original_object_type': entry.kind,
            }
            if source.includes(entry.path):
                record['original_git_blob_oid'] = entry.oid
                data = (root / source.destination / entry.path).read_bytes()
                if blob_oid(data) != entry.oid:
                    raise ProvenanceError('BLOB_MISMATCH', entry.path)
                notices = [line.strip() for line in data.decode('utf-8', errors='replace').splitlines()
                           if NOTICE.search(line)]
                record['sha256'] = sha256(data)
                record['notice_lines'] = list(notices)
                copyright_lines.update(line for line in notices if 'copyright' in line.casefold())
                files.append(record)
            else:
                record['reason'] = 'outside audited proto/LICENSE/README reference subset'
                excluded.append(record)
        components.append({
            'name': source.name, 'upstream_url': source.url, 'upstream_commit': source.pin,
            'upstream_tree': source.tree, 'destination': source.destination,
            'upstream_ls_tree_sha256': source.listing_sha256,
            'vendored_count': len(files), 'excluded_count': len(excluded),
            'files': files, 'excluded': excluded,
        })
    document: JsonValue = {
        'schema': 'aa-provenance-2', 'components': components,
        'copyright_lines': dict(sorted(copyright_lines.items())),
        'downstream_patches': [],
    }
    return json.dumps(document, indent=2, ensure_ascii=True) + '\n'


if __name__ == '__main__':
    print(render(Path(sys.argv[1]).resolve()), end='')
