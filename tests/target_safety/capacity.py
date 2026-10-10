# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/capacity.py
"""Tiny-fixture counting-limit qualification with substituted small caps."""
from __future__ import annotations

import os
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage, inventory as inventory_module, kernel
from target_safety.coverage import Scope, protect
from target_safety.inventory import inventory
from target_safety.kernel import Blocked, Watch


def entry_fixture(files: int) -> tuple[Path, Scope]:
    """Two roots, approved link and file entries; counted objects = 4 + files."""
    fixture = Path(tempfile.mkdtemp(prefix="capacity-entries-", dir="/tmp/opencode"))
    repo, external = fixture / "repo", fixture / "external"
    repo.mkdir()
    external.mkdir()
    (repo / ".codegraph").symlink_to(external)
    (external / "index").write_bytes(b"x")
    for index in range(files):
        (repo / f"f{index}").write_bytes(b"x")
    return fixture, Scope(repo, external, repo / ".codegraph")


def entry_limit_exact_passes() -> None:
    """Given exactly N counted objects, When protecting at cap N, Then reach inventory."""
    fixture, scope = entry_fixture(2)
    n = 6
    with patch.object(coverage, "PROTECTED_ENTRY_LIMIT", n):
        with Watch.session() as watch:
            paths = protect(watch, scope).paths
            rows = inventory(watch, paths)
    assert len(paths) == n
    assert len(rows) == n
    print(f"PASS capacity entry-limit exact N={n} passes into inventory; fixture={fixture}")


def entry_limit_n_minus_one_blocks_before_inventory() -> None:
    """Given the same N objects, When protecting at cap N-1, Then block before inventory."""
    fixture, scope = entry_fixture(2)
    n = 6
    reached_inventory = False
    try:
        with patch.object(coverage, "PROTECTED_ENTRY_LIMIT", n - 1):
            with Watch.session() as watch:
                paths = protect(watch, scope).paths
                reached_inventory = True
                inventory(watch, paths)
    except Blocked as error:
        assert error.reason == "entry_limit", str(error)
        assert not reached_inventory
        assert watch.closed
        print(f"PASS capacity entry-limit N-1 cap blocks before inventory: {error}; fixture={fixture}")
        return
    raise AssertionError("cap N-1 accepted N objects")


def entry_limit_one_beyond_blocks_before_inventory() -> None:
    """Given N+1 counted objects at cap N, When protecting, Then block before inventory."""
    fixture, scope = entry_fixture(3)
    n = 6
    reached_inventory = False
    try:
        with patch.object(coverage, "PROTECTED_ENTRY_LIMIT", n):
            with Watch.session() as watch:
                paths = protect(watch, scope).paths
                reached_inventory = True
                inventory(watch, paths)
    except Blocked as error:
        assert error.reason == "entry_limit", str(error)
        assert not reached_inventory
        assert watch.closed
        print(f"PASS capacity entry-limit one-beyond blocks before inventory: {error}; fixture={fixture}")
        return
    raise AssertionError("one-beyond entry count accepted")


def watch_limit_distinct_then_next_rejected() -> None:
    """Given W distinct watches at cap W, When adding the next distinct target, Then reject."""
    fixture = Path(tempfile.mkdtemp(prefix="capacity-watches-", dir="/tmp/opencode"))
    w = 3
    targets = []
    for index in range(w + 1):
        path = fixture / f"o{index}"
        path.write_bytes(b"x")
        targets.append(path)
    try:
        with patch.object(kernel, "MAX_WATCHES", w):
            with Watch.session() as watch:
                for path in targets[:w]:
                    watch.add(path)
                assert len(watch.paths) == w
                watch.add(targets[w])
    except Blocked as error:
        assert error.reason == "watch_limit", str(error)
        assert watch.closed
        print(f"PASS capacity watch-limit W={w} distinct pass, next rejected: {error}; fixture={fixture}")
        return
    raise AssertionError("watch beyond the distinct limit accepted")


def shared_descriptor_merges_filters_on_one_watch() -> None:
    """Given a hardlink to a watched inode, When added with another entry filter, Then merge."""
    fixture = Path(tempfile.mkdtemp(prefix="capacity-shared-", dir="/tmp/opencode"))
    first = fixture / "first"
    first.write_bytes(b"x")
    alias = fixture / "alias"
    os.link(first, alias)
    with Watch.session() as watch:
        watch.add(first, "alpha")
        watch.add(alias, "beta")
        assert len(watch.paths) == 1
        assert watch.filters[next(iter(watch.filters))] == {"alpha", "beta"}
    print(f"PASS capacity shared-descriptor filter merge on one watch; fixture={fixture}")


def at_cap_alias_rejected_with_cleanup() -> None:
    # Given: two names for one real inode and a one-descriptor application cap.
    with tempfile.TemporaryDirectory(prefix='capacity-alias-', dir='/tmp/opencode') as directory:
        first, alias = Path(directory) / 'first', Path(directory) / 'alias'
        first.write_bytes(b'x')
        os.link(first, alias)
        try:
            with patch.object(kernel, 'MAX_WATCHES', 1), Watch.session() as watch:
                watch.add(first, 'alpha')
                # When: an alias is added after the registry reaches the cap.
                watch.add(alias, 'beta')
        except Blocked as error:
            # Then: reject conservatively, keep filters intact and close the FD.
            assert error.reason == 'watch_limit'
            assert len(watch.paths) == 1
            assert watch.filters[next(iter(watch.filters))] == {'alpha'}
            assert watch.closed
            try:
                os.fstat(watch.fd)
            except OSError as closed:
                assert closed.errno == 9
            else:
                raise AssertionError('watch descriptor leaked')
        else:
            raise AssertionError('at-cap alias accepted')
    print('PASS real-kernel at-cap alias conservatively rejected with descriptor cleanup')


def aggregate_fixture(extra: bool) -> tuple[Path, Scope, int]:
    """Files plus an in-tree hardlink alias; counted bytes = 4 + 3 + 4 + 1 (+1)."""
    fixture = Path(tempfile.mkdtemp(prefix="capacity-bytes-", dir="/tmp/opencode"))
    repo, external = fixture / "repo", fixture / "external"
    repo.mkdir()
    external.mkdir()
    (repo / ".codegraph").symlink_to(external)
    (external / "index").write_bytes(b"i")
    (repo / "a").write_bytes(b"AAAA")
    (repo / "b").write_bytes(b"BBB")
    os.link(repo / "a", repo / "alias")
    counted = 4 + 3 + 4 + 1
    if extra:
        (repo / "c").write_bytes(b"C")
        counted += 1
    return fixture, Scope(repo, external, repo / ".codegraph"), counted


def aggregate_bytes_exact_with_hardlink_passes() -> None:
    """Given counted bytes B including the alias pathname, When at cap B, Then complete."""
    fixture, scope, counted = aggregate_fixture(extra=False)
    assert counted == 12
    with patch.object(inventory_module, "INVENTORY_BYTE_LIMIT", counted):
        with Watch.session() as watch:
            rows = inventory(watch, protect(watch, scope).paths)
    assert len(rows) == 7
    print(f"PASS capacity aggregate bytes B={counted} with hardlink path count passes; fixture={fixture}")


def aggregate_bytes_alias_counted_blocks_at_b_minus_one() -> None:
    """Given the same tree, When at cap B-1, Then block: the alias pathname bytes count."""
    fixture, scope, counted = aggregate_fixture(extra=False)
    try:
        with patch.object(inventory_module, "INVENTORY_BYTE_LIMIT", counted - 1):
            with Watch.session() as watch:
                inventory(watch, protect(watch, scope).paths)
    except Blocked as error:
        assert error.reason == "inventory_byte_limit", str(error)
        assert watch.closed
        print(f"PASS capacity hardlink alias bytes counted, cap B-1 blocks: {error}; fixture={fixture}")
        return
    raise AssertionError("deduplicated hardlink bytes accepted at cap B-1")


def aggregate_bytes_one_beyond_blocks() -> None:
    """Given B+1 counted bytes at cap B, When inventorying, Then block on byte excess."""
    fixture, scope, counted = aggregate_fixture(extra=True)
    assert counted == 13
    try:
        with patch.object(inventory_module, "INVENTORY_BYTE_LIMIT", counted - 1):
            with Watch.session() as watch:
                inventory(watch, protect(watch, scope).paths)
    except Blocked as error:
        assert error.reason == "inventory_byte_limit", str(error)
        assert watch.closed
        print(f"PASS capacity aggregate bytes B+1 blocks: {error}; fixture={fixture}")
        return
    raise AssertionError("B+1 aggregate bytes accepted at cap B")


def exhaustive_object_coverage() -> None:
    """Given directory/.git/vendor/ignored-style objects and an inode alias, Then cover all."""
    fixture = Path(tempfile.mkdtemp(prefix="capacity-coverage-", dir="/tmp/opencode"))
    repo, external = fixture / "repo", fixture / "external"
    repo.mkdir()
    external.mkdir()
    (repo / ".codegraph").symlink_to(external)
    (external / "index").write_bytes(b"i")
    (repo / "nested").mkdir()
    (repo / "nested/data").write_bytes(b"d")
    (repo / ".git").mkdir()
    (repo / ".git/config").write_bytes(b"g")
    (repo / "vendor").mkdir()
    (repo / "vendor/lib.c").write_bytes(b"v")
    (repo / "build-output.o").write_bytes(b"o")
    os.link(repo / "nested/data", repo / "alias-link")
    with Watch.session() as watch:
        covered = {str(path) for path in protect(watch, Scope(repo, external, repo / ".codegraph")).paths}
        for expected in (repo, external, repo / ".codegraph", repo / "nested", repo / "nested/data",
                         repo / ".git", repo / ".git/config", repo / "vendor", repo / "vendor/lib.c",
                         repo / "build-output.o", repo / "alias-link", external / "index"):
            assert str(expected) in covered, expected
        assert {row.path for row in inventory(watch, tuple(sorted(Path(p) for p in covered)))} == covered
    print(f"PASS capacity exhaustive directory/.git/vendor/ignored/alias coverage; fixture={fixture}")


def main() -> int:
    entry_limit_exact_passes()
    entry_limit_n_minus_one_blocks_before_inventory()
    entry_limit_one_beyond_blocks_before_inventory()
    watch_limit_distinct_then_next_rejected()
    shared_descriptor_merges_filters_on_one_watch()
    at_cap_alias_rejected_with_cleanup()
    aggregate_bytes_exact_with_hardlink_passes()
    aggregate_bytes_alias_counted_blocks_at_b_minus_one()
    aggregate_bytes_one_beyond_blocks()
    exhaustive_object_coverage()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
