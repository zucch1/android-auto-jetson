# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_sqlite_fixtures.py
"""Exact SQLite watched-absence fixture topology and fixed-policy substitution.

Shared by the sqlite-absence contract suites. The production constants are
substituted explicitly (Oracle design): unrelated-root fixtures without this
substitution keep pre-existing behavior and carry null wire evidence.
"""
from __future__ import annotations

import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage
from target_safety.coverage import Scope

LINK_TEXT = "../sqlite3.h"


def topology(case: str) -> tuple[Path, Path, Path, Path, tuple[Path, Path]]:
    """Private two-root fixture carrying the exact dangling link and absent target."""
    root = Path(tempfile.mkdtemp(prefix=f"sqlite-absence-{case}-", dir="/tmp/opencode"))
    repo, external = root / "repository", root / "external"
    repo.mkdir()
    external.mkdir()
    (repo / "input.txt").write_text("fixture input\n")
    (repo / "nested").mkdir()
    (repo / "nested/data.bin").write_bytes(b"fixture data")
    (external / "graph.txt").write_text("fixture graph\n")
    (repo / ".codegraph").symlink_to(external)
    namespaces = (repo / ".local_env/aqt-venv/bin/python3", repo / ".venv/bin/python3")
    for namespace in namespaces:
        namespace.parent.mkdir(parents=True)
        namespace.symlink_to("/usr/bin/python3")
    link = repo / ".local_env/navigation/tools/valhalla-deps/usr/include/spatialite/sqlite3.h"
    link.parent.mkdir(parents=True)
    link.symlink_to(LINK_TEXT)
    target = repo / ".local_env/navigation/tools/valhalla-deps/usr/include/sqlite3.h"
    return repo, external, link, target, namespaces


@contextmanager
def fixed_policy(repo: Path, external: Path, link: Path, target: Path,
                 namespaces: tuple[Path, Path], *, namespace_only: bool = True
                 ) -> Iterator[Scope]:
    """Substitute the exact fixed policy constants; yields the production-shaped scope."""
    with patch.multiple(coverage, REPOSITORY=repo, EXTERNAL=external,
                        SQLITE_LINK=link, SQLITE_LINK_TEXT=LINK_TEXT,
                        SQLITE_TARGET=target, NAMESPACE_LINKS=namespaces):
        yield Scope(repo, external, repo / ".codegraph", namespace_only=namespace_only)


def exact_record(link: Path, target: Path) -> dict:
    """The exact policy record for this fixture, field for field."""
    return {"link": str(link), "literal_link_text": LINK_TEXT, "target": str(target),
            "link_parent": str(link.parent), "target_parent": str(target.parent),
            "enoent_observed": True, "link_watched": True,
            "link_parent_watched": True, "target_parent_watched": True}
