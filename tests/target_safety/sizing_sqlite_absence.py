# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_sqlite_absence.py
"""Exact owner-approved SQLite watched-absence contract; real fs, real inotify.

Covers exactness (literal text, normalization, exact link), strict ENOENT
absence (only FileNotFoundError+ENOENT accepted), real-parent proof (root
through both parents, present in the walk, real directories, identity equal),
alias rejection (direct/transitive/suffix hit the exempt link; Python
namespace aliases unchanged) and the final-boundary recheck. Deterministic
hooks only; no sleeps; private fixtures only.
"""
from __future__ import annotations

import errno
import os
import stat
import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage, kernel
from sizing_sqlite_fixtures import LINK_TEXT, exact_record, fixed_policy, topology


def _protect(repo, external, link, target, namespaces, **kwargs):
    with fixed_policy(repo, external, link, target, namespaces, **kwargs) as scope:
        with kernel.Watch.session() as watch:
            covered = coverage.protect(watch, scope)
            return scope, watch, covered


def exact_success_produces_the_typed_record() -> None:
    # Given: the exact dangling link, literal text and absent target. Then: the
    # complete walk accepts and carries the typed exact record, nothing else.
    repo, external, link, target, namespaces = topology("exact")
    _, _, covered = _protect(repo, external, link, target, namespaces)
    assert covered.sqlite_absence == exact_record(link, target)
    print("PASS exact watched-absence success carries the typed exact record")


def wrong_literal_with_same_normalized_target_rejects() -> None:
    # Given: link text normalizing to the exact target but spelled differently.
    repo, external, link, target, namespaces = topology("wrong-text")
    link.unlink()
    link.symlink_to(".././sqlite3.h")
    try:
        _protect(repo, external, link, target, namespaces)
    except kernel.Blocked as error:
        assert error.reason == "sqlite_link_text_mismatch", error
    else:
        raise AssertionError("wrong literal text must reject")
    print("PASS wrong literal text with same normalized target rejects")


def sibling_and_foreign_dangling_links_reject() -> None:
    # Given: the exact link plus one dangling sibling or foreign link at a time.
    for case, extra in (("sibling", "spatialite/other.h"), ("foreign", "foreign.h")):
        repo, external, link, target, namespaces = topology(f"aliases-{case}")
        dangling = repo / ".local_env/navigation/tools/valhalla-deps/usr/include" / extra
        dangling.parent.mkdir(parents=True, exist_ok=True)
        dangling.symlink_to("../nowhere.h")
        try:
            _protect(repo, external, link, target, namespaces)
        except kernel.Blocked:
            pass
        else:
            raise AssertionError(f"{case} dangling link must reject")
    print("PASS sibling and foreign dangling links reject without broad acceptance")


def missing_or_nonlink_expected_link_rejects() -> None:
    # Given: the approved link missing or replaced by a regular file. Then:
    # reject; deletion and replacement are contract breaches, never absence.
    for state in ("missing", "nonlink"):
        repo, external, link, target, namespaces = topology(f"link-{state}")
        link.unlink()
        if state == "nonlink":
            link.write_text("not a link\n")
        try:
            _protect(repo, external, link, target, namespaces)
        except kernel.Blocked as error:
            assert error.reason == "sqlite_link_missing_or_nonlink", error
        else:
            raise AssertionError(f"{state} expected link must reject")
    print("PASS missing or non-link expected link rejects")


def target_present_regular_directory_or_dangling_symlink_rejects() -> None:
    # Given: the approved target exists. Then: reject every kind of entry.
    for kind in ("file", "directory", "dangling"):
        repo, external, link, target, namespaces = topology(f"target-{kind}")
        if kind == "file":
            target.write_bytes(b"appeared")
        elif kind == "directory":
            target.mkdir()
        else:
            target.symlink_to("../absent-referent.h")
        try:
            _protect(repo, external, link, target, namespaces)
        except kernel.Blocked as error:
            assert error.reason == "sqlite_target_present", error
        else:
            raise AssertionError(f"target present as {kind} must reject")
    print("PASS target present as file/directory/dangling symlink rejects")


def _lstat_fault(target: Path, error: BaseException):
    real = os.lstat

    def routed(path, *args, **kwargs):
        if os.fspath(path) == os.fspath(target):
            raise error
        return real(path, *args, **kwargs)

    return patch("os.lstat", side_effect=routed)


def target_lstat_accepts_only_enoent() -> None:
    # Given: lstat on the exact target fails. Then: only FileNotFoundError with
    # errno ENOENT proves absence; every other shape blocks.
    faults = {
        "PermissionError": PermissionError(errno.EACCES, "denied"),
        "ENOTDIR": OSError(errno.ENOTDIR, "not a directory"),
        "IOError": OSError(errno.EIO, "io"),
        "FileNotFoundError wrong errno": FileNotFoundError(errno.EACCES, "spoofed"),
    }
    for case, error in faults.items():
        repo, external, link, target, namespaces = topology("lstat-block")
        try:
            with _lstat_fault(target, error):
                _protect(repo, external, link, target, namespaces)
        except kernel.Blocked as blocked:
            assert blocked.reason == "sqlite_target_absence_unproven", (case, blocked)
        else:
            raise AssertionError(f"{case} must block absence")
    print("PASS target lstat accepts only FileNotFoundError+ENOENT")


def parent_proof_rejects_symlink_missing_and_identity_races() -> None:
    # Given: crafted walk evidence where a root-through-parent component is a
    # symlink, missing, or its current identity differs from the saved one.
    repo, external, link, target, namespaces = topology("parents")
    include = target.parent
    with fixed_policy(repo, external, link, target, namespaces) as scope:
        with kernel.Watch.session() as watch:
            for watched_path in (link, link.parent, target.parent, scope.repository):
                watch.add(watched_path)
            seen = {scope.repository: coverage.identity(scope.repository)}
            for path in scope.repository.rglob("*"):
                seen[path] = coverage.identity(path)
            watched = dict(seen)
            link_parent_info = seen[link.parent]
            seen[link.parent] = (link_parent_info[0], link_parent_info[1], stat.S_IFLNK | 0o777,
                                 *link_parent_info[4:])
            try:
                coverage.sqlite_absence_check(watch, scope, seen)
            except kernel.Blocked as error:
                assert error.reason == "sqlite_parent_not_real_directory", error
            else:
                raise AssertionError("symlink parent must reject")
            broken = dict(watched)
            broken.pop(include)
            try:
                coverage.sqlite_absence_check(watch, scope, broken)
            except kernel.Blocked as error:
                assert error.reason == "sqlite_parent_uncovered", error
            else:
                raise AssertionError("uncovered component must reject")
            raced = dict(watched)
            raced[scope.repository] = (0, 0, stat.S_IFDIR | 0o755, 0, 0, 0, 0, 0, 0)
            try:
                coverage.sqlite_absence_check(watch, scope, raced)
            except kernel.Blocked as error:
                assert error.reason == "sqlite_parent_identity_changed", error
            else:
                raise AssertionError("root identity race must reject")
            coverage.sqlite_absence_check(watch, scope, dict(watched))
    print("PASS parent proof rejects symlink, uncovered and identity-race components")


def direct_transitive_and_suffix_aliases_reject() -> None:
    # Given: other links whose chains hit the exempt SQLite link. Then: all
    # reject; the owner approved exactly one origin link, never an alias family.
    for case in ("direct", "transitive", "suffix"):
        repo, external, link, target, namespaces = topology(f"alias-{case}")
        alias = repo / "alias.h"
        to_link = os.path.relpath(link, alias.parent)
        if case == "direct":
            alias.symlink_to(to_link)
        elif case == "transitive":
            (repo / "hop.h").symlink_to(to_link)
            alias.symlink_to("./hop.h")
        else:
            alias.symlink_to(f"{to_link}/extra")
        try:
            _protect(repo, external, link, target, namespaces)
        except kernel.Blocked as error:
            assert error.reason in ("sqlite_alias_not_exempt", "uncovered_link_target",
                                    "namespace_link_not_directory"), (case, error)
        else:
            raise AssertionError(f"{case} alias must reject")
    print("PASS direct, transitive and suffix aliases of the exempt link reject")


def python_namespace_aliases_keep_existing_contract() -> None:
    # Given: an alias ending exactly at an approved namespace link. Then: that
    # pre-existing acceptance is unchanged; its suffix case still rejects.
    repo, external, link, target, namespaces = topology("namespace-alias")
    alias = repo / "python-alias"
    alias.symlink_to(".venv/bin/python3")
    _protect(repo, external, link, target, namespaces)
    suffix = repo / "python-suffix"
    suffix.symlink_to(".venv/bin/python3/extra")
    try:
        _protect(repo, external, link, target, namespaces)
    except kernel.Blocked as error:
        assert error.reason == "namespace_link_not_directory", error
    else:
        raise AssertionError("namespace suffix alias must reject")
    print("PASS Python namespace alias acceptance and suffix rejection unchanged")


def boundary_recheck_reproduces_the_record_or_rejects() -> None:
    # Given: a successful window. Then: the final-boundary recheck reproduces
    # the retained record; after a real mutation the boundary rejects, whether
    # the observation or the queued window reports it first.
    repo, external, link, target, namespaces = topology("recheck")
    with fixed_policy(repo, external, link, target, namespaces) as scope:
        with kernel.Watch.session() as watch:
            covered = coverage.protect(watch, scope)
            assert covered.sqlite_absence == exact_record(link, target)
            coverage.sqlite_absence_recheck(watch, scope, covered.sqlite_absence)
        caught: list[str] = []
        try:
            with kernel.Watch.session() as watch:
                covered = coverage.protect(watch, scope)
                target.write_bytes(b"appeared")
                try:
                    coverage.sqlite_absence_recheck(watch, scope, covered.sqlite_absence)
                except kernel.Blocked as error:
                    caught.append(error.reason)
                else:
                    raise AssertionError("boundary recheck must reject a created target")
        except kernel.Blocked as error:
            caught.append(error.reason)
        assert set(caught) & {"sqlite_target_present", "watch_violation", "window_invalidated"}, caught
    print("PASS boundary recheck reproduces the record and rejects mutation")


def inventory_keeps_literal_text_digest_and_absent_target() -> None:
    # Given: the covered walk. Then: the link is inventoried with the digest of
    # its literal text and the target is absent from the inventory.
    import hashlib
    from target_safety.inventory import inventory
    repo, external, link, target, namespaces = topology("inventory")
    with fixed_policy(repo, external, link, target, namespaces) as scope:
        with kernel.Watch.session() as watch:
            covered = coverage.protect(watch, scope)
            rows = inventory(watch, covered.paths)
    by_path = {row.path: row for row in rows}
    digest = hashlib.sha256(os.fsencode(LINK_TEXT)).hexdigest()
    assert by_path[str(link)].sha256 == digest
    assert str(target) not in by_path
    print("PASS inventory keeps the literal-text digest and no target row")


def main() -> int:
    exact_success_produces_the_typed_record()
    wrong_literal_with_same_normalized_target_rejects()
    sibling_and_foreign_dangling_links_reject()
    missing_or_nonlink_expected_link_rejects()
    target_present_regular_directory_or_dangling_symlink_rejects()
    target_lstat_accepts_only_enoent()
    parent_proof_rejects_symlink_missing_and_identity_races()
    direct_transitive_and_suffix_aliases_reject()
    python_namespace_aliases_keep_existing_contract()
    boundary_recheck_reproduces_the_record_or_rejects()
    inventory_keeps_literal_text_digest_and_absent_target()
    print("PASS all sqlite watched-absence contract suites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
