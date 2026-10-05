# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_manifest.py
"""Manifest assembly and offline-materializer consumption: observed provenance,
fixture rejection, symlink closure, and the exact stage.materialize boundary."""
from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.acquire import (
    AcquisitionRecord,
    CopiedEntry,
    PackageInfo,
    ProcessObservation,
    QuietBoundary,
    TargetIdentity,
)
from target_safety.acquire_manifest import build_manifest, write_manifest
from tools.sysroot.manifest import encode_manifest, parse_manifest
from tools.sysroot.models import Code, PackageName, SysrootError, require_observed
from tools.sysroot.stage import materialize, validate_artifact

QUIET = "process observations are not a no-writers proof"


def _pkg(name: str = "libfixture", arch: str = "arm64", source: str = "fixture-src") -> PackageInfo:
    return PackageInfo(name, "1.0-1", arch, source)


def _record(base: Path, provenance: str = "observed",
            dangling: bool = False) -> tuple[AcquisitionRecord, Path]:
    snapshot = base / "snapshot"
    (snapshot / "usr/include").mkdir(parents=True)
    (snapshot / "usr/lib").mkdir(parents=True)
    (snapshot / "usr/include/f.h").write_bytes(b"header\n")
    (snapshot / "usr/lib/libf.so.1").write_bytes(b"libbytes\n")
    os.symlink("libf.so.1", snapshot / "usr/lib/libf.so")
    def sha(b: bytes) -> str:
        return hashlib.sha256(b).hexdigest()
    pkg = _pkg()
    entries = [
        CopiedEntry("/usr/include/f.h", "regular", "usr/include/f.h",
                    sha(b"header\n"), len(b"header\n"), None, pkg),
    ]
    if not dangling:
        entries.append(CopiedEntry("/usr/lib/libf.so.1", "regular", "usr/lib/libf.so.1",
                                   sha(b"libbytes\n"), len(b"libbytes\n"), None, pkg))
    entries.append(CopiedEntry("/usr/lib/libf.so", "symlink", "usr/lib/libf.so",
                               sha(b"libf.so.1"), None, "libf.so.1", pkg))
    record = AcquisitionRecord(
        "aa-acquire/1", "win-1", provenance,
        TargetIdentity("6.8.12", "R39.2.1", "5.1", (("libfixture", "1.0-1"),)),
        tuple(entries),
        QuietBoundary(ProcessObservation((), None), ProcessObservation((), None),
                      (), "quiescent", True, QUIET),
        str(base))
    return record, snapshot


def test_materializer_consumes_observed_output() -> None:
    base = Path(tempfile.mkdtemp(prefix="am-observed-", dir="/tmp/opencode"))
    record, snapshot = _record(base)
    manifest_path = base / "manifest.json"
    manifest = write_manifest(record, "jetson-r39.2.1", manifest_path)
    assert manifest.provenance.value == "observed"
    assert [p.name for p in manifest.packages] == ["libfixture"]
    assert {type(f).__name__ for f in manifest.files} == {"RegularFile", "Symlink"}
    require_observed(manifest)

    out = base / "out"
    out.mkdir()
    staged = materialize(snapshot, manifest_path, out)
    artifact = validate_artifact(out / "jetson-r39.2.1")
    require_observed(artifact.manifest)
    assert staged.manifest_sha256 == artifact.manifest_sha256
    print("PASS offline materializer consumes observed route output (validate + require-observed)")


def test_fixture_provenance_rejected() -> None:
    base = Path(tempfile.mkdtemp(prefix="am-fixture-", dir="/tmp/opencode"))
    record, _ = _record(base, provenance="fixture")
    manifest = build_manifest(record, "jetson-r39.2.1")
    try:
        require_observed(manifest)
    except SysrootError as error:
        assert error.code == Code.FIXTURE_PROVENANCE, error
    else:
        raise AssertionError("require_observed must reject fixture provenance")
    print("PASS require_observed rejects fixture provenance (target evidence must be observed)")


def test_symlink_closure_enforced() -> None:
    base = Path(tempfile.mkdtemp(prefix="am-closure-", dir="/tmp/opencode"))
    record, _ = _record(base, dangling=True)
    manifest = build_manifest(record, "jetson-r39.2.1")
    blob = encode_manifest(manifest)
    try:
        parse_manifest(blob)
    except SysrootError as error:
        assert error.code == Code.SYMLINK_CLOSURE, error
    else:
        raise AssertionError("parse_manifest must reject a dangling acquired symlink")
    print("PASS symlink closure enforced: an acquired symlink needs its referent declared")


def test_arch_all_requires_justification() -> None:
    base = Path(tempfile.mkdtemp(prefix="am-arch-", dir="/tmp/opencode"))
    record, _ = _record(base)
    entries = tuple(
        CopiedEntry(e.original_input_path, e.kind, e.copied_path, e.sha256, e.size,
                    e.link_text, _pkg(arch="all", source="arch-src"))
        for e in record.entries)
    record = AcquisitionRecord(record.schema, record.task_window_id, record.provenance,
                               record.identity, entries, record.quiet_boundary, record.scratch)
    manifest = build_manifest(record, "jetson-r39.2.1")
    only = manifest.packages[0]
    assert only.architecture.value == "all" and only.all_justification
    print("PASS architecture=all build input carries an explicit all_justification")


def main() -> int:
    test_multiarch_observed_identity()
    test_materializer_consumes_observed_output()
    test_fixture_provenance_rejected()
    test_symlink_closure_enforced()
    test_arch_all_requires_justification()
    return 0


def test_multiarch_observed_identity() -> None:
    base = Path(tempfile.mkdtemp(prefix="am-multiarch-", dir="/tmp/opencode"))
    record, snapshot = _record(base)
    package = PackageInfo("libasan8:arm64", "14.2.0-4ubuntu2~24.04.1", "arm64", "gcc-14")
    record = replace(record, entries=tuple(replace(e, package=package) for e in record.entries))
    metadata = base / "manifest.json"
    assembled = write_manifest(record, "multiarch-v1", metadata)
    parsed = parse_manifest(metadata.read_bytes())
    assert parsed == assembled
    assert parsed.packages[0].name == package.name
    assert parsed.packages[0].version == package.version
    assert parsed.packages[0].source == package.source
    assert all(e.package == package.name for e in parsed.files)
    destination = base / "out"
    destination.mkdir()
    staged = materialize(snapshot, metadata, destination)
    assert validate_artifact(staged.artifact).manifest == parsed
    for name in ("libasan8:amd64", "libasan8:", "libasan8:arm64:arm64", "libasan8/arm64"):
        invalid = replace(parsed, packages=(replace(parsed.packages[0], name=PackageName(name)),))
        try:
            parse_manifest(encode_manifest(invalid))
        except SysrootError as error:
            assert error.code == Code.MALFORMED_METADATA, error
        else:
            raise AssertionError(f"accepted invalid package identity: {name}")
    print("PASS exact dpkg multiarch identity survives assembly, validation and staging")


if __name__ == "__main__":
    raise SystemExit(main())
