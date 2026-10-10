# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tools/build/drift.py --baseline toolchains/jetson-sysroot-manifest.json --candidate other.json
"""Detect package identity and same-version content drift; require a new sysroot version on change."""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.manifest import manifest_digest, parse_manifest
from tools.sysroot.models import Code, Digest, Manifest, Provenance, Version


@dataclass(frozen=True, slots=True)
class DriftReport:
    baseline_version: Version
    candidate_version: Version
    baseline_digest: Digest
    candidate_digest: Digest
    package_version_drift: bool
    content_drift: bool
    same_version_content_drift: bool
    new_version_required: bool
    changed_packages: tuple[str, ...]
    changed_files: tuple[str, ...]
    acceptance: str


def acceptance(provenance: Provenance) -> str:
    return 'target-observed' if provenance is Provenance.OBSERVED else 'fixture-not-target'


def compare(baseline: Manifest, candidate: Manifest) -> DriftReport:
    base_packages = {p.name: p for p in baseline.packages}
    cand_packages = {p.name: p for p in candidate.packages}
    changed_packages = tuple(sorted(
        name for name in base_packages.keys() | cand_packages.keys()
        if base_packages.get(name) != cand_packages.get(name)))
    base_files = {str(f.staged_path): f.sha256 for f in baseline.files}
    cand_files = {str(f.staged_path): f.sha256 for f in candidate.files}
    changed_files = tuple(sorted(
        path for path in base_files.keys() | cand_files.keys()
        if base_files.get(path) != cand_files.get(path)))
    package_version_drift = bool(changed_packages)
    content_drift = bool(changed_files)
    same_version = baseline.version == candidate.version
    return DriftReport(
        baseline.version, candidate.version, manifest_digest(baseline),
        manifest_digest(candidate), package_version_drift, content_drift,
        same_version and (package_version_drift or content_drift),
        package_version_drift or content_drift,
        changed_packages, changed_files, acceptance(baseline.provenance))


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = arguments()
    report = compare(parse_manifest(args.baseline.read_bytes()),
                     parse_manifest(args.candidate.read_bytes()))
    document = asdict(report)
    document['code'] = Code.NEW_VERSION_REQUIRED if report.new_version_required else None
    print(json.dumps(document))
    return 1 if report.new_version_required else 0


if __name__ == '__main__':
    raise SystemExit(main())
