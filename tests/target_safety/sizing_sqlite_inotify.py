# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_sqlite_inotify.py
"""Real inotify boundary windows for the SQLite watched-absence contract.

Deterministic hooks (watch-registration and scan-order wrappers), never
sleeps. A transient target create+remove after the parent watch must reject
from the queue whether the hook fires at parent or at link registration and
under both scan orders; every post-protect mutation must invalidate the final
boundary.
"""
from __future__ import annotations

import os
import sys
from contextlib import contextmanager
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage, kernel
from sizing_sqlite_fixtures import fixed_policy, topology


def _run_with_hook(repo, external, link, target, namespaces, hook, scan_reverse=False):
    real_add = kernel.Watch.add
    real_scandir = os.scandir

    def hooked_add(self, path, entry=None):
        real_add(self, path, entry)
        hook(Path(path))

    @contextmanager
    def ordered_scandir(path):
        with real_scandir(path) as entries:
            ordered = sorted(entries, key=lambda entry: entry.name, reverse=scan_reverse)
        yield ordered

    with fixed_policy(repo, external, link, target, namespaces) as scope:
        with patch.object(kernel.Watch, "add", hooked_add), \
                patch("os.scandir", ordered_scandir), kernel.Watch.session() as watch:
            return coverage.protect(watch, scope)


def transient_create_remove_rejects_from_the_queue() -> None:
    # Given: the target created and removed right after its parent watch is
    # registered (parent-first) or right after the link watch (link-side).
    # Then: both walk orders reject; the transient window is never accepted.
    for trigger_at, scan_reverse in (("parent", False), ("link", True),
                                     ("parent", True), ("link", False)):
        repo, external, link, target, namespaces = topology(f"transient-{trigger_at}-{scan_reverse}")
        fired: list[str] = []

        def hook(path: Path) -> None:
            watched = target.parent if trigger_at == "parent" else link
            if path == watched and not fired:
                fired.append(str(path))
                target.write_bytes(b"transient")
                target.unlink()

        try:
            _run_with_hook(repo, external, link, target, namespaces, hook, scan_reverse)
        except kernel.Blocked:
            pass
        else:
            raise AssertionError(f"transient create+remove at {trigger_at}/{scan_reverse} must reject")
        assert fired, "hook must have fired deterministically"
    print("PASS transient target create+remove after parent/link watch rejects in both walk orders")


def both_scan_orders_keep_the_exact_contract() -> None:
    # Given: the clean exact fixture. Then: forward and reverse scan orders
    # both accept it with the same typed record.
    for scan_reverse in (False, True):
        repo, external, link, target, namespaces = topology(f"order-{scan_reverse}")
        covered = _run_with_hook(repo, external, link, target, namespaces,
                                 lambda path: None, scan_reverse)
        assert covered.sqlite_absence is not None
        assert covered.sqlite_absence["link"] == str(link)
    print("PASS both scan orders keep the exact contract")


def post_protect_mutations_reject_at_the_final_boundary() -> None:
    # Given: a clean window past protect(). Then: every mutation kind must be
    # rejected at the final boundary: created target, sibling entry, replaced
    # parents and a retargeted or replaced link.
    cases = ("target-create", "sibling", "link-parent-replace", "target-parent-replace",
             "link-retarget", "link-replace")
    for case in cases:
        repo, external, link, target, namespaces = topology(f"post-{case}")
        caught: list[str] = []
        try:
            with fixed_policy(repo, external, link, target, namespaces) as scope:
                with kernel.Watch.session() as watch:
                    covered = coverage.protect(watch, scope)
                    coverage.sqlite_absence_recheck(watch, scope, covered.sqlite_absence)
                    if case == "target-create":
                        target.write_bytes(b"appeared")
                    elif case == "sibling":
                        (target.parent / "sibling.h").write_bytes(b"appeared")
                    elif case == "link-parent-replace":
                        os.rename(link.parent, link.parent.with_name("spatialite-old"))
                        link.parent.mkdir()
                    elif case == "target-parent-replace":
                        os.rename(target.parent, target.parent.with_name("include-old"))
                        target.parent.mkdir()
                    elif case == "link-retarget":
                        os.rename(link, link.with_name("sqlite3.h.old"))
                        link.symlink_to("../other.h")
                    else:
                        os.rename(link, link.with_name("sqlite3.h.old"))
                        link.write_text("replaced\n")
                    try:
                        coverage.sqlite_absence_recheck(watch, scope, covered.sqlite_absence)
                    except kernel.Blocked as error:
                        caught.append(error.reason)
                    else:
                        raise AssertionError(f"{case} must be rejected by the boundary recheck")
        except kernel.Blocked as error:
            caught.append(error.reason)
        assert set(caught) & {"sqlite_target_present", "sqlite_link_text_mismatch",
                              "sqlite_link_missing_or_nonlink", "sqlite_parent_missing",
                              "sqlite_parent_identity_changed", "sqlite_absence_changed",
                              "watch_violation", "window_invalidated",
                              "sqlite_parent_not_real_directory"}, (case, caught)
    print("PASS post-protect target/sibling/parent/link mutations reject at the final boundary")


def main() -> int:
    transient_create_remove_rejects_from_the_queue()
    both_scan_orders_keep_the_exact_contract()
    post_protect_mutations_reject_at_the_final_boundary()
    print("PASS all sqlite watched-absence inotify boundary suites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
