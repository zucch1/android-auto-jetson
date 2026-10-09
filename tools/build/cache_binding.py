# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tools/build/cache_binding.py --cache-root .local/build-cache --manifest toolchains/jetson-sysroot-manifest.json
"""Bind an immutable private build cache to a manifest digest; never silently reuse an old cache."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import os
from pathlib import Path
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.manifest import manifest_digest, parse_manifest
from tools.sysroot.models import (Code, Digest, Manifest, Provenance, SysrootError, Version,
                                  require_observed)


@dataclass(frozen=True, slots=True)
class Binding:
    cache_root: Path
    namespace: Path
    manifest_digest: Digest
    version: Version
    provenance: Provenance
    acceptance: str
    namespace_reused: bool
    current_rebound: bool


def acceptance(provenance: Provenance) -> str:
    return 'target-observed' if provenance is Provenance.OBSERVED else 'fixture-not-target'


def _record(manifest: Manifest, digest: Digest) -> dict[str, str]:
    return {'manifest_digest': digest, 'version': manifest.version,
            'provenance': manifest.provenance, 'acceptance': acceptance(manifest.provenance)}


def _write_once(path: Path, record: dict[str, str]) -> bool:
    if path.exists():
        existing = json.loads(path.read_text())
        if existing != record:
            raise SysrootError(Code.NEW_VERSION_REQUIRED, str(path))
        return True
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, sort_keys=True))
    path.chmod(0o444)
    return False


def bind(cache_root: Path, manifest: Manifest) -> Binding:
    """Namespace cache bytes by digest; a same-version digest change is a hard drift gate."""
    digest = manifest_digest(manifest)
    namespace = cache_root / digest
    namespace_reused = _write_once(namespace / 'binding.json', _record(manifest, digest))
    current = cache_root / 'current.json'
    rebound = False
    if current.exists():
        previous = json.loads(current.read_text())
        if previous['manifest_digest'] != digest:
            if previous['version'] == manifest.version:
                raise SysrootError(Code.NEW_VERSION_REQUIRED, 'current.same_version')
            current.unlink()
            current.write_text(json.dumps(_record(manifest, digest), sort_keys=True))
            rebound = True
    else:
        current.write_text(json.dumps(_record(manifest, digest), sort_keys=True))
    return Binding(cache_root, namespace, digest, manifest.version, manifest.provenance,
                   acceptance(manifest.provenance), namespace_reused, rebound)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache-root', type=Path, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--require-observed', action='store_true',
                        help='reject fixture provenance for real target cache qualification')
    return parser.parse_args()


def main() -> int:
    args = arguments()
    try:
        manifest = parse_manifest(args.manifest.read_bytes())
        if args.require_observed:
            require_observed(manifest)
        result = bind(args.cache_root, manifest)
    except SysrootError as error:
        print(json.dumps({'code': error.code, 'field': error.field}), file=sys.stderr)
        return 1
    except OSError as error:
        print(json.dumps({'code': Code.LOCAL_IO, 'errno': error.errno}), file=sys.stderr)
        return 1
    print(json.dumps({'manifest_digest': result.manifest_digest, 'version': result.version,
                      'provenance': result.provenance, 'namespace': str(result.namespace),
                      'namespace_reused': result.namespace_reused,
                      'current_rebound': result.current_rebound,
                      'acceptance': result.acceptance}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
