# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tools/build/validate_sysroot.py --manifest toolchains/jetson-sysroot-manifest.json --payload .local/sysroots/jetson-r39.2.1/rootfs
"""Validate every recorded sysroot input hash before compiling; no authenticity or target claim."""
from __future__ import annotations

import argparse
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import stat
import sys
from typing import assert_never

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.filesystem import directory, parent, regular
from tools.sysroot.manifest import manifest_digest, parse_manifest
from tools.sysroot.models import (Code, Digest, Manifest, RegularFile, Provenance, SysrootError,
                                  Symlink, Version, require_observed)
from tools.sysroot.stage import validate_artifact


def acceptance(provenance: Provenance) -> str:
    return 'target-observed' if provenance is Provenance.OBSERVED else 'fixture-not-target'


def validate_hashes(payload: Path, manifest: Manifest) -> None:
    """Re-verify each recorded content/link hash descriptor-relative, never following links."""
    with ExitStack() as stack:
        root = directory(payload, stack)
        for entry in manifest.files:
            path = str(entry.staged_path)
            name = path.split('/')[-1]
            match entry:
                case RegularFile():
                    stream = regular(root, path, stack)
                    if hashlib.file_digest(stream, 'sha256').hexdigest() != entry.sha256:
                        raise SysrootError(Code.HASH_MISMATCH, path)
                case Symlink(link_text=link):
                    descriptor = parent(root, path, stack)
                    info = os.stat(name, dir_fd=descriptor, follow_symlinks=False)
                    if (not stat.S_ISLNK(info.st_mode)
                            or os.readlink(name, dir_fd=descriptor) != link):
                        raise SysrootError(Code.HASH_MISMATCH, path)
                case unreachable:
                    assert_never(unreachable)


def validate_payload(manifest_path: Path, payload: Path) -> tuple[Manifest, Digest]:
    manifest = parse_manifest(manifest_path.read_bytes())
    validate_hashes(payload, manifest)
    return manifest, manifest_digest(manifest)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--artifact', type=Path, help='frozen stage.materialize output directory')
    mode.add_argument('--manifest', type=Path, help='external aa-sysroot-1 manifest.json')
    parser.add_argument('--payload', type=Path, help='rootfs payload root paired with --manifest')
    parser.add_argument('--require-observed', action='store_true',
                        help='reject fixture provenance for real target acceptance')
    return parser.parse_args()


def main() -> int:
    args = arguments()
    try:
        if args.artifact is not None:
            staged = validate_artifact(args.artifact)
            manifest, digest = staged.manifest, staged.manifest_sha256
        else:
            if args.manifest is None or args.payload is None:
                raise SysrootError(Code.LOCAL_IO, 'manifest/payload pairing')
            manifest, digest = validate_payload(args.manifest, args.payload)
        if args.require_observed:
            require_observed(manifest)
    except SysrootError as error:
        print(json.dumps({'code': error.code, 'field': error.field}), file=sys.stderr)
        return 1
    except OSError as error:
        print(json.dumps({'code': Code.LOCAL_IO, 'errno': error.errno}), file=sys.stderr)
        return 1
    print(json.dumps({'manifest_sha256': digest, 'version': manifest.version,
                      'provenance': manifest.provenance, 'files': len(manifest.files),
                      'packages': len(manifest.packages),
                      'acceptance': acceptance(manifest.provenance)}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
