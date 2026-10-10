# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/build/decode_overlay.py --root ROOT --manifest MANIFEST [--digest SHA256]
"""Snapshot or verify the complete decode overlay; no target qualification implied."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
from pathlib import Path
import re
import stat
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.models import Code, SysrootError


@dataclass(frozen=True, slots=True)
class Overlay:
    root: Path
    manifest: Path
    digest: str


def snapshot(root: Path) -> bytes:
    """Hash all regular-file content, including contained file links and dotfiles."""
    base = root.resolve(strict=True)
    records: list[str] = []
    for path in sorted(base.rglob('*')):
        relative = path.relative_to(base).as_posix()
        resolved = path.resolve(strict=True)
        if not resolved.is_relative_to(base) or re.search(r'[^A-Za-z0-9_./+-]', relative):
            raise SysrootError(Code.ARTIFACT_MISMATCH, f'AA_DECODE_PKG_ESCAPE:{relative}')
        if path.is_dir():
            # pathlib does not traverse directory links: refuse unenumerated subtrees.
            if path.is_symlink():
                raise SysrootError(Code.ARTIFACT_MISMATCH, f'AA_DECODE_PKG_ESCAPE:{relative}')
            continue
        if not stat.S_ISREG(resolved.stat().st_mode):
            raise SysrootError(Code.ARTIFACT_MISMATCH, f'AA_DECODE_PKG_ESCAPE:{relative}')
        with resolved.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        records.append(f'{digest}  {relative}\n')
    return ''.join(records).encode('ascii')


def verify(overlay: Overlay) -> frozenset[Path]:
    """Bind manifest bytes and exact current file set to the configured digest."""
    expected = overlay.manifest.read_bytes()
    if (overlay.root.resolve(strict=True) != overlay.root
            or hashlib.sha256(expected).hexdigest() != overlay.digest
            or snapshot(overlay.root) != expected):
        raise SysrootError(Code.ARTIFACT_MISMATCH, 'AA_DECODE_OVERLAY_MISMATCH')
    return frozenset(overlay.root / line.split('  ', 1)[1]
                     for line in expected.decode('ascii').splitlines())


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--digest')
    args = parser.parse_args()
    try:
        if args.digest is None:
            data = snapshot(args.root)
            args.manifest.write_bytes(data)
            print(hashlib.sha256(data).hexdigest())
        else:
            verify(Overlay(args.root, args.manifest, args.digest))
            print(f'AA_DECODE_OVERLAY_VERIFIED:{args.digest}')
    except (OSError, SysrootError) as error:
        print(f'AA_DECODE_OVERLAY_MISMATCH: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
