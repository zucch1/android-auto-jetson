# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Import tools.sysroot.models; CLI: python3 -B tools/sysroot/cli.py --help
"""Offline integrity models; observed is a declaration, not authenticated evidence."""
from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import NewType, assert_never

Version = NewType('Version', str)
Digest = NewType('Digest', str)
PackageName = NewType('PackageName', str)
RelativePath = NewType('RelativePath', str)
InputPath = NewType('InputPath', str)


class Code(StrEnum):
    MALFORMED_METADATA = 'MALFORMED_METADATA'
    UNSAFE_PATH = 'UNSAFE_PATH'
    DUPLICATE = 'DUPLICATE'
    PACKAGE_OWNER = 'PACKAGE_OWNER'
    SYMLINK_CLOSURE = 'SYMLINK_CLOSURE'
    INVENTORY_MISMATCH = 'INVENTORY_MISMATCH'
    HASH_MISMATCH = 'HASH_MISMATCH'
    ARTIFACT_MISMATCH = 'ARTIFACT_MISMATCH'
    NEW_VERSION_REQUIRED = 'NEW_VERSION_REQUIRED'
    FIXTURE_PROVENANCE = 'FIXTURE_PROVENANCE'
    LOCAL_IO = 'LOCAL_IO'


@dataclass(frozen=True, slots=True)
class SysrootError(Exception):
    code: Code
    field: str

    def __str__(self) -> str:
        return f'{self.code}\t{self.field}'


class Provenance(StrEnum):
    FIXTURE = 'fixture'
    OBSERVED = 'observed'


class Architecture(StrEnum):
    ARM64 = 'arm64'
    ALL = 'all'


@dataclass(frozen=True, slots=True)
class Identity:
    kernel: str | None
    l4t: str | None


@dataclass(frozen=True, slots=True)
class Package:
    name: PackageName
    version: str
    architecture: Architecture
    source: str
    all_justification: str | None


@dataclass(frozen=True, slots=True)
class RegularFile:
    original_input_path: InputPath
    staged_path: RelativePath
    package: PackageName
    sha256: Digest


@dataclass(frozen=True, slots=True)
class Symlink:
    original_input_path: InputPath
    staged_path: RelativePath
    package: PackageName
    sha256: Digest
    link_text: str


type Entry = RegularFile | Symlink


@dataclass(frozen=True, slots=True)
class Manifest:
    version: Version
    provenance: Provenance
    identity: Identity
    packages: tuple[Package, ...]
    files: tuple[Entry, ...]


def require_observed(manifest: Manifest) -> Manifest:
    """Reject fixtures for future integration; does not qualify a target or authenticate it."""
    match manifest.provenance:
        case Provenance.FIXTURE:
            raise SysrootError(Code.FIXTURE_PROVENANCE, 'provenance')
        case Provenance.OBSERVED:
            return manifest
        case unreachable:
            assert_never(unreachable)
