# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble the `aa-sysroot-1` manifest from an observed acquisition record.

Local-only driver step (never bundled into the target payload): it reuses the
existing tools.sysroot models and manifest encoder to turn the raw acquisition
record (AcquisitionRecord) into a typed sysroot Manifest whose provenance is the
record's observed declaration, then encodes it exactly as the offline
materializer (tools.sysroot.stage.materialize) expects. Safe staging is per the
models: staged_path mirrors the original input path, symlink closure is checked
by the model parser, and every regular input / symlink literal text digest is
the one observed during acquisition.
"""
from __future__ import annotations

from pathlib import Path
from typing import assert_never

import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.manifest import encode_manifest, manifest_digest
from tools.sysroot.models import (
    Architecture,
    Code,
    Digest,
    Identity,
    InputPath,
    Manifest,
    Package,
    PackageName,
    Provenance,
    RegularFile,
    RelativePath,
    Symlink,
    SysrootError,
    Version,
)

from .acquire_models import AcquisitionRecord, CopiedEntry


def _package_for(entry: CopiedEntry, seen: dict[str, Package]) -> None:
    info = entry.package
    try:
        arch = Architecture(info.architecture)
    except ValueError as error:
        raise SysrootError(Code.MALFORMED_METADATA, f"package.architecture:{info.architecture}") from error
    source = info.source or info.name
    match arch:
        case Architecture.ARM64:
            justification = None
        case Architecture.ALL:
            justification = f"arch-independent build input {info.name} from {source}"
        case unreachable:  # pragma: no cover - exhaustive Architecture
            assert_never(unreachable)
    existing = seen.get(info.name)
    if existing is not None and existing.version != info.version:
        raise SysrootError(Code.DUPLICATE, f"package.version:{info.name}")
    seen.setdefault(info.name, Package(PackageName(info.name), info.version, arch, source,
                                       justification))


def _entry_for(entry: CopiedEntry) -> RegularFile | Symlink:
    staged = RelativePath(entry.original_input_path[1:])
    owner = PackageName(entry.package.name)
    if entry.kind == "regular":
        return RegularFile(InputPath(entry.original_input_path), staged, owner,
                           Digest(entry.sha256))
    if entry.kind == "symlink":
        link = entry.link_text or ""
        return Symlink(InputPath(entry.original_input_path), staged, owner,
                       Digest(entry.sha256), link)
    raise SysrootError(Code.MALFORMED_METADATA, f"file.kind:{entry.kind}")


def build_manifest(record: AcquisitionRecord, version: str) -> Manifest:
    """Typed sysroot Manifest from the observed record; provenance is observed/fixture."""
    try:
        provenance = Provenance(record.provenance)
    except ValueError as error:
        raise SysrootError(Code.MALFORMED_METADATA, "provenance") from error
    packages: dict[str, Package] = {}
    for entry in record.entries:
        _package_for(entry, packages)
    files = tuple(_entry_for(entry) for entry in record.entries)
    return Manifest(
        version=Version(version),
        provenance=provenance,
        identity=Identity(record.identity.kernel, record.identity.l4t),
        packages=tuple(sorted(packages.values(), key=lambda p: p.name)),
        files=tuple(sorted(files, key=lambda f: f.staged_path)),
    )


def write_manifest(record: AcquisitionRecord, version: str, path: Path) -> Manifest:
    """Encode the aa-sysroot-1 manifest and retain it at path; returns the model."""
    manifest = build_manifest(record, version)
    path.write_bytes(encode_manifest(manifest))
    return manifest


def record_manifest_sha256(record: AcquisitionRecord, version: str) -> str:
    return manifest_digest(build_manifest(record, version))
