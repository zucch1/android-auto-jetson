# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/namespace.py
"""Exact-policy and real-kernel namespace-only qualification; retain fixtures."""
from __future__ import annotations

import hashlib
import os
import sys
import tempfile
from dataclasses import replace
from pathlib import Path
from typing import Literal, assert_never
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage
from target_safety.coverage import Scope, protect
from target_safety.inventory import inventory
from target_safety.kernel import Blocked, Watch

Case = Literal["allowed", "wrong-text", "wrong-literal", "wrong-path", "missing", "regular",
               "replacement", "parent-replacement", "referent-write", "alias", "alias-suffix", "codegraph-write"]
Slot = Literal["aqt", "venv"]
SLOTS: tuple[Slot, Slot] = ("aqt", "venv")
CASES: tuple[Case, ...] = ("allowed", "wrong-text", "wrong-literal", "wrong-path", "missing", "regular",
                           "replacement", "parent-replacement", "referent-write", "alias", "alias-suffix",
                           "codegraph-write")


def scenario(case: Case, slot: Slot) -> None:
    """Given isolated policy fixtures, When guarded, Then enforce namespace limits."""
    fixture = Path(tempfile.mkdtemp(prefix="namespace-", dir="/tmp/opencode"))
    repo, external = fixture / "repo", fixture / "external"
    repo.mkdir()
    external.mkdir()
    (external / "data").write_bytes(b"protected")
    (repo / ".codegraph").symlink_to(external)
    parents = {"aqt": repo / ".local_env/aqt-venv/bin", "venv": repo / ".venv/bin"}
    for parent in parents.values():
        parent.mkdir(parents=True)
    links = {name: parent / "python3" for name, parent in parents.items()}
    link = links[slot]
    other = links["venv" if slot == "aqt" else "aqt"]
    referent = fixture / "interpreter"
    referent.write_bytes(b"excluded")
    text = str(referent) if case == "referent-write" else "/usr/bin/python3"
    match case:
        case "missing":
            other.symlink_to(text)
            assert not link.is_symlink()
        case "regular":
            other.symlink_to(text)
            link.write_bytes(b"not a link")
        case "wrong-text":
            other.symlink_to(text)
            link.symlink_to("/usr/bin/./python3")
        case "wrong-literal":
            other.symlink_to(text)
            link.symlink_to("/bin/python3")
        case "wrong-path":
            other.symlink_to(text)
            (link.parent / "python").symlink_to(text)
            (link.parent / "python3.11").symlink_to(text)
            link.symlink_to(text)
        case "alias":
            other.symlink_to(text)
            link.symlink_to(text)
            (repo / "alias").symlink_to(link.relative_to(repo))
        case "alias-suffix":
            other.symlink_to(text)
            link.symlink_to(text)
            (repo / "alias").symlink_to(link.relative_to(repo).as_posix() + "/child")
        case "allowed" | "replacement" | "parent-replacement" | "referent-write" | "codegraph-write":
            other.symlink_to(text)
            link.symlink_to(text)
        case unreachable:
            assert_never(unreachable)
    established = False
    # Substitute only the fixed approval constants for private kernel fixtures.
    with patch.object(coverage, "NAMESPACE_LINKS", (links["aqt"], links["venv"])), \
            patch.object(coverage, "NAMESPACE_TEXT", text):
        scope = replace(Scope(repo, external, repo / ".codegraph"), namespace_only=True)
        try:
            with Watch.session() as watch:
                paths = protect(watch, scope).paths
                before = inventory(watch, paths)
                assert referent not in paths and Path("/usr/bin/python3") not in paths
                digest = hashlib.sha256(os.fsencode(text)).hexdigest()
                assert next(row.sha256 for row in before if row.path == str(links["aqt"])) == digest
                assert next(row.sha256 for row in before if row.path == str(links["venv"])) == digest
                established = True
                # When: operate on private fixtures only, without deletion.
                match case:
                    case "replacement":
                        link.rename(link.parent / "old-python3")
                        link.write_bytes(b"replacement")
                    case "parent-replacement":
                        link.parent.rename(link.parent.with_name("old-bin"))
                        link.parent.mkdir()
                    case "referent-write":
                        referent.write_bytes(b"excluded mutation must not invalidate")
                    case "codegraph-write":
                        (external / "data").write_bytes(b"blocked mutation")
                    case "allowed" | "alias":
                        assert link in paths and other in paths
                    case "wrong-text" | "wrong-literal" | "wrong-path" | "missing" | "regular" | "alias-suffix":
                        raise AssertionError("invalid namespace accepted")
                    case unreachable:
                        assert_never(unreachable)
                assert inventory(watch, paths) == before
        except Blocked as error:
            assert case not in ("allowed", "alias", "referent-write"), str(error)
            assert established == (case in ("replacement", "parent-replacement", "codegraph-write")), str(error)
            match case:
                case "missing" | "regular":
                    assert error.reason == "namespace_link_missing_or_nonlink", str(error)
                    assert str(link) in error.detail, str(error)
                case "wrong-text" | "wrong-literal":
                    assert error.reason == "namespace_link_text_mismatch", str(error)
                    assert str(link) in error.detail, str(error)
                case "wrong-path":
                    assert error.reason == "unapproved_external_link", str(error)
                case "alias-suffix":
                    assert error.reason == "namespace_link_not_directory", str(error)
                case "replacement" | "parent-replacement" | "codegraph-write":
                    assert error.reason == "window_invalidated", str(error)
                case "allowed" | "alias" | "referent-write":
                    raise AssertionError(str(error))
                case unreachable:
                    assert_never(unreachable)
            assert watch.closed
            print(f"PASS namespace {case} {slot}: {error}; fixture={fixture}")
            return
    assert case in ("allowed", "alias", "referent-write")
    assert watch.closed
    print(f"PASS namespace {case} {slot}; fixture={fixture}")


def main() -> int:
    # Given production policy, When reading literal text, Then accept only the exact pair.
    assert coverage.NAMESPACE_LINKS == (
        Path("/home/zucchi/Desktop/infotainment-plan/.local_env/aqt-venv/bin/python3"),
        Path("/home/zucchi/Desktop/infotainment-plan/.venv/bin/python3"),
    )
    assert coverage.NAMESPACE_TEXT == "/usr/bin/python3"
    assert hashlib.sha256(os.fsencode(coverage.NAMESPACE_TEXT)).hexdigest() == \
        "31f2aee4e71d21fbe5cf8b01ff0e069b9275f58929596ceb00d14d90e3e16cd6"
    repository = Path("/home/zucchi/Desktop/infotainment-plan")
    external = Path("/home/zucchi/.omo/codegraph/projects/infotainment-plan-68328f6064505b10")
    scope = replace(Scope(repository, external, repository / ".codegraph"), namespace_only=True)
    with patch.object(coverage.os, "readlink", return_value="/usr/bin/python3"):
        for exact in coverage.NAMESPACE_LINKS:
            assert coverage.confined_target(exact, scope) is None
            for sibling in ("python", "python3.11"):
                try:
                    coverage.confined_target(exact.with_name(sibling), scope)
                except Blocked as error:
                    assert error.reason == "unapproved_external_link"
                else:
                    raise AssertionError("production sibling accepted")
    open_scope = replace(scope, namespace_only=False)
    with patch.object(coverage.os, "readlink", return_value="/usr/bin/python3"):
        for exact in coverage.NAMESPACE_LINKS:
            try:
                coverage.confined_target(exact, open_scope)
            except Blocked as error:
                assert error.reason == "unapproved_external_link"
            else:
                raise AssertionError("namespace_only=False accepted external interpreter link")
    print("PASS production exact-pair and sibling rejection")
    for slot in SLOTS:
        for case in CASES:
            scenario(case, slot)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
