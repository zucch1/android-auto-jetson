# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Imported by python3 -B tests/build/scenarios.py ROOT CASE
"""Synthetic build fixtures for task-5 acceptance checks; never real target observations."""
from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.manifest import encode_manifest
from tools.sysroot.models import (Architecture, Digest, Identity, InputPath, Manifest, Package,
                                  PackageName, Provenance, RegularFile, RelativePath, Symlink,
                                  Version)
from tools.sysroot.stage import materialize

# Mirrors the proven tests/sysroot/fixtures leaf layout so parse_manifest closure is satisfied.
REGULAR_PATH = 'usr/share/data'
LINK_PATH = 'usr/share/alias'
LINK_TEXT = 'data'
CONTENT = b'synthetic bytes\n'
TAMPERED = b'tampered bytes\n'


def build_manifest(version: str, package_version: str, content: bytes,
                   source: str = 'synthetic-local',
                   architecture: Architecture = Architecture.ARM64,
                   all_justification: str | None = None) -> Manifest:
    package = Package(PackageName('fixture-data'), package_version, architecture,
                      source, all_justification)
    entries = (
        RegularFile(InputPath('/' + REGULAR_PATH), RelativePath(REGULAR_PATH),
                    PackageName('fixture-data'), Digest(hashlib.sha256(content).hexdigest())),
        Symlink(InputPath('/' + LINK_PATH), RelativePath(LINK_PATH), PackageName('fixture-data'),
                Digest(hashlib.sha256(LINK_TEXT.encode()).hexdigest()), LINK_TEXT),
    )
    return Manifest(Version(version), Provenance.FIXTURE, Identity('synthetic', None),
                    (package,), entries)


def write_manifest(path: Path, manifest: Manifest) -> None:
    path.write_bytes(encode_manifest(manifest))


def write_payload(root: Path, content: bytes) -> None:
    (root / REGULAR_PATH).parent.mkdir(parents=True, exist_ok=True)
    (root / REGULAR_PATH).write_bytes(content)
    (root / LINK_PATH).symlink_to(LINK_TEXT)


def frozen_artifact(base: Path, version: str, content: bytes) -> Path:
    """Return a frozen stage.materialize artifact (the locked sysroot) for cache tests."""
    snapshot = base / 'snapshot'
    metadata = base / 'metadata.json'
    destination = base / 'output'
    write_payload(snapshot, content)
    metadata.write_bytes(encode_manifest(build_manifest(version, '1.0-1', content)))
    destination.mkdir()
    return materialize(snapshot, metadata, destination).artifact


def compile_binary(compiler: str, output: Path, source: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([compiler, '-std=c++20', '-O2', '-o', str(output), str(source)],
                          capture_output=True, text=True, check=False)


def run_tool(root: Path, tool: str, *args: str) -> subprocess.CompletedProcess[str]:
    command = [sys.executable, '-B', str(root / 'tools/build' / tool), *args]
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    print(f'{tool} exit={result.returncode}')
    print(result.stdout + result.stderr, end='')
    return result
