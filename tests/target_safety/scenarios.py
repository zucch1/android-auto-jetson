# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/scenarios.py
"""Real Linux watch qualification; fixtures are retained without deletion."""
from __future__ import annotations

import ctypes
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Literal, TypeAlias, assert_never

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.coverage import Scope, protect
from target_safety.kernel import Blocked, Watch
from target_safety.inventory import inventory
from target_safety.stage1 import bundle

Case: TypeAlias = Literal["baseline", "internal-link", "other-referent", "external-chain",
                          "hardlink-write", "transient-write", "referent-hardlink-write", "lost-watch",
                          "link-replacement", "root-replacement", "ancestor-replacement", "new-directory",
                          "file-replacement", "unrelated-parent-entry"]


def scenario(case: Case) -> None:
    """Given two isolated roots, When one action occurs, Then enforce coverage."""
    fixture = Path(tempfile.mkdtemp(prefix=f"target-safety-{case}-", dir="/tmp/opencode"))
    repo, external = fixture / "repo", fixture / "external"
    repo.mkdir()
    external.mkdir()
    (repo / "nested").mkdir()
    data = repo / "nested/data"
    data.write_bytes(b"baseline")
    (external / "index").write_bytes(b"approved")
    link = repo / ".codegraph"
    link.symlink_to(external)
    alias = fixture / "outside-hardlink"
    os.link(data, alias)
    os.link(external / "index", fixture / "external-alias")
    if case == "other-referent":
        (repo / "unapproved").symlink_to(fixture)
    if case == "internal-link":
        (repo / "internal").symlink_to("nested/data")
    if case == "external-chain":
        (external / "escape").symlink_to(fixture)
        (repo / "chain").symlink_to(external / "escape/repo")
    scope = Scope(repo, external, link)
    established = False
    try:
        with Watch.session() as watch:
            paths = protect(watch, scope).paths
            before = inventory(watch, paths)
            assert next(row.sha256 for row in before if row.path == str(data)) == hashlib.sha256(b"baseline").hexdigest()
            assert next(row.sha256 for row in before if row.path == str(external / "index")) == hashlib.sha256(b"approved").hexdigest()
            established = True
            # When: mutate only the private fixture, never a real protected path.
            match case:
                case "baseline" | "internal-link":
                    assert inventory(watch, paths) == before
                case "hardlink-write":
                    alias.write_bytes(b"changed through outside inode alias")
                case "transient-write":
                    data.write_bytes(b"temporary")
                    data.write_bytes(b"baseline")
                case "referent-hardlink-write":
                    (fixture / "external-alias").write_bytes(b"outside referent write")
                case "lost-watch":
                    watch.libc.inotify_rm_watch.argtypes = [ctypes.c_int, ctypes.c_int]
                    watch.libc.inotify_rm_watch.restype = ctypes.c_int
                    assert watch.libc.inotify_rm_watch(watch.fd, next(iter(watch.paths))) == 0
                case "link-replacement":
                    link.rename(repo / "old-link")
                    link.symlink_to(external)
                case "root-replacement":
                    external.rename(fixture / "old-external")
                    external.mkdir()
                case "ancestor-replacement":
                    fixture.rename(fixture.with_name(fixture.name + "-moved"))
                case "new-directory":
                    (external / "new").mkdir()
                case "file-replacement":
                    data.rename(repo / "nested/old-data")
                    data.write_bytes(b"replacement")
                case "unrelated-parent-entry":
                    (fixture / "unrelated").write_bytes(b"not protected")
                case "other-referent" | "external-chain":
                    raise AssertionError("unapproved referent accepted")
                case unreachable:
                    assert_never(unreachable)
            watch.check()
    except Blocked as error:
        assert case not in ("baseline", "internal-link", "unrelated-parent-entry"), str(error)
        if case in ("other-referent", "external-chain"):
            assert error.reason == "unapproved_external_link", str(error)
        else:
            assert established and error.reason == "window_invalidated", str(error)
        assert watch.closed
        print(f"PASS real-kernel {case}: {error}; fixture={fixture}")
        return
    assert case in ("baseline", "internal-link", "unrelated-parent-entry"), case
    print(f"PASS real-kernel {case}; fixture={fixture}")


def parser_case(mask: int) -> None:
    """Given synthetic kernel bytes, When parsed, Then reject lost coverage."""
    try:
        with Watch.session() as watch:
            watch.consume(struct.pack("iIII", -1, mask, 0, 0))
    except Blocked as error:
        assert watch.closed and watch.violations[0].mask == mask
        print(f"PASS synthetic-parser mask={mask:#x}: {error}")
        return
    raise AssertionError("coverage-loss packet accepted")


def main() -> int:
    from namespace import main as namespace_main
    from receipt import main as receipt_main
    namespace_main()
    receipt_main()
    for case in ("baseline", "internal-link", "other-referent", "external-chain",
                 "hardlink-write", "transient-write", "referent-hardlink-write", "lost-watch", "link-replacement",
                 "root-replacement", "ancestor-replacement", "new-directory",
                 "file-replacement", "unrelated-parent-entry"):
        scenario(case)
    for mask in (0x4000, 0x8000, 0x2000):
        parser_case(mask)
    for mutation in (False, True):
        stdin_scenario(mutation)
    from capacity import main as capacity_main
    from envelope import main as envelope_main
    from window import main as window_main
    capacity_main()
    window_main()
    envelope_main()
    return 0


def stdin_scenario(mutation: bool) -> None:
    """Given bundled stdin, When a full window runs, Then expose pass/blocker."""
    fixture = Path(tempfile.mkdtemp(prefix="target-safety-stdin-", dir="/tmp/opencode"))
    (fixture / "repo").mkdir()
    (fixture / "external").mkdir()
    (fixture / "repo/.codegraph").symlink_to(fixture / "external")
    (fixture / "external/data").write_bytes(b"initial")
    entry = f'''
from pathlib import Path
from target_safety.coverage import Scope, protect
from target_safety.inventory import inventory
from target_safety.kernel import Blocked, Watch
f = Path({str(fixture)!r})
try:
    with Watch.session() as w:
        paths = protect(w, Scope(f / 'repo', f / 'external', f / 'repo/.codegraph')).paths
        before = inventory(w, paths)
        if {mutation!r}:
            (f / 'external/data').write_bytes(b'changed')
        after = inventory(w, paths)
        assert before == after
    print('window-pass-after-cleanup')
except Blocked as error:
    print(error)
    raise SystemExit(70)
'''
    payload = bundle().rsplit(b"raise SystemExit", 1)[0] + entry.encode()
    result = subprocess.run([sys.executable, "-B", "-"], input=payload,
                            capture_output=True, check=False, timeout=10)
    assert result.returncode == (70 if mutation else 0), result.stderr
    assert (b"window_invalidated" if mutation else b"window-pass-after-cleanup") in result.stdout
    print(f"PASS stdin-e2e mutation={mutation} exit={result.returncode}; fixture={fixture}")


if __name__ == "__main__":
    raise SystemExit(main())
