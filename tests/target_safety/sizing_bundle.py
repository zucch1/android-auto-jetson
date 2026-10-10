# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_bundle.py
"""Real sizing bundle stdin execution: topology only, compact outcome, fake authority.

The real sizing guard payload (no inventory/probes modules) and the real
supervisor payload run through python3 -B - with only the setting authority and
the protected roots substituted. No SSH, no kernel write, no target contact.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage, stage1
from target_safety.payload import sizing_guard_bundle
from target_safety.report import accepted_ceiling_output, accepted_sizing_output
from ceiling_setting_fixtures import fake_setting

SQLITE_TEXT = "../sqlite3.h"


def sqlite_objects(repo: Path) -> tuple[Path, Path]:
    link = repo / ".local_env/navigation/tools/valhalla-deps/usr/include/spatialite/sqlite3.h"
    return link, link.parent.parent / "sqlite3.h"


def substitute_policy(repo: Path, external: Path):
    link, target = sqlite_objects(repo)
    return patch.multiple(coverage, REPOSITORY=repo, EXTERNAL=external,
                          SQLITE_LINK=link, SQLITE_LINK_TEXT=SQLITE_TEXT, SQLITE_TARGET=target)


def fixture_roots(root: Path) -> tuple[Path, Path, tuple[Path, Path]]:
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
    link, _ = sqlite_objects(repo)
    link.parent.mkdir(parents=True)
    link.symlink_to(SQLITE_TEXT)
    return repo, external, namespaces


def payloads(root: Path, *, entry_limit: int | None = None,
             mem_available: int | None = None) -> bytes:
    repo, external, namespaces = fixture_roots(root)
    setting, state = fake_setting(root)
    guard = sizing_guard_bundle()
    guard_prefix = guard.rsplit(b"raise SystemExit", 1)[0]
    limit = f"_C.PROTECTED_ENTRY_LIMIT = {entry_limit}\n" if entry_limit is not None else ""
    admission = (f"import target_safety.bootstrap as _B\n"
                 f"_B.read_mem_available = lambda: {mem_available}\n") if mem_available is not None else ""
    seams = f"""
import sys
assert 'target_safety.inventory' not in sys.modules and 'target_safety.probes' not in sys.modules
import target_safety.sizing as _Z, target_safety.coverage as _C
from pathlib import Path
_Z.REPOSITORY = Path({str(repo)!r})
_Z.EXTERNAL = Path({str(external)!r})
_C.REPOSITORY = _Z.REPOSITORY
_C.EXTERNAL = _Z.EXTERNAL
_C.NAMESPACE_LINKS = (Path({str(namespaces[0])!r}), Path({str(namespaces[1])!r}))
_Z.NAMESPACE_LINKS = _C.NAMESPACE_LINKS
_C.SQLITE_LINK = Path({str(sqlite_objects(repo)[0])!r})
_C.SQLITE_LINK_TEXT = {SQLITE_TEXT!r}
_C.SQLITE_TARGET = Path({str(sqlite_objects(repo)[1])!r})
{limit}""".encode()
    fixture_guard = guard_prefix + seams + b"raise SystemExit(target_safety.sizing.main())\n"
    prefix = stage1.sizing_bundle().rsplit(b"raise SystemExit", 1)[0]
    injection = f"""
from pathlib import Path as _P
import target_safety.coverage as _VC
_VC.REPOSITORY = _P({str(repo)!r}); _VC.EXTERNAL = _P({str(external)!r})
_VC.SQLITE_LINK = _P({str(sqlite_objects(repo)[0])!r}); _VC.SQLITE_LINK_TEXT = {SQLITE_TEXT!r}
_VC.SQLITE_TARGET = _P({str(sqlite_objects(repo)[1])!r})
from target_safety.setting import SudoSetting
_setting = SudoSetting(sudo={setting.sudo!r}, sysctl={setting.sysctl!r})
{admission}_S.GUARD_BUNDLE = {fixture_guard!r}
raise SystemExit(_S.main(setting=_setting, mode='topology-sizing'))
""".encode()
    return prefix + injection


def run_window(root: Path, **kwargs) -> subprocess.CompletedProcess[bytes]:
    result = subprocess.run((sys.executable, "-B", "-"), input=payloads(root, **kwargs),
                            capture_output=True, timeout=90, check=False)
    (root / "stdout.txt").write_bytes(result.stdout)
    (root / "stderr.txt").write_bytes(result.stderr)
    return result


def sizing_bundle_succeeds_with_compact_counters() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-bundle-", dir="/tmp/opencode"))
    result = run_window(root)
    assert result.returncode == 0, result.stderr or result.stdout
    stdout = result.stdout.decode()
    with substitute_policy(root / "repository", root / "external"):
        assert accepted_sizing_output(stdout), stdout
    assert not accepted_ceiling_output(stdout)
    outcome = next(json.loads(line) for line in stdout.splitlines()
                   if json.loads(line).get("event") == "sizing_outcome")["data"]
    assert outcome["mode"] == "topology-sizing" and outcome["guard_exit"] == 0
    sizing = outcome["sizing"]
    kinds = sizing["kinds"]
    assert sizing["discovered"] == sizing["processed"] and sizing["pending"] == 0
    assert kinds["directories"] + kinds["regular_files"] + kinds["symlinks"] == sizing["discovered"]
    assert sizing["watch_descriptors"] >= sizing["discovered"] > 0
    assert sizing["path_bytes"] > 0 and sizing["regular_file_bytes"] > 0
    assert (sizing["inventory_complete"], sizing["acquisition"], sizing["task5_completed"]) == (False, False, False)
    assert outcome["resources"]["samples"] >= 1 and outcome["collection"]["invalidated"] is False
    assert "guard_stdout" not in outcome and "guard_stderr" not in outcome
    assert "child_limit_ready" in stdout and "topology_sizing_complete" in stdout
    assert (root / "state").read_text() == "65536"
    assert len(json.dumps(outcome)) < 4096
    print(f"PASS sizing bundle success: compact counters {sizing['discovered']} objects; fixture={root}")


def sizing_bundle_cap_failure_blocks() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-bundle-cap-", dir="/tmp/opencode"))
    result = run_window(root, entry_limit=3)
    assert result.returncode == 70, result.stdout
    stdout = result.stdout.decode()
    with substitute_policy(root / "repository", root / "external"):
        assert not accepted_sizing_output(stdout) and not accepted_ceiling_output(stdout)
    assert "guard_blocked" in stdout and "entry_limit" in stdout
    assert "topology_sizing_complete" not in stdout
    assert (root / "state").read_text() == "65536"
    print(f"PASS sizing bundle cap failure blocks acceptance and restores; fixture={root}")


def sizing_guard_payload_carries_no_inventory_or_probes() -> None:
    guard = sizing_guard_bundle()
    for name in ("inventory", "probes"):
        assert f"types.ModuleType('target_safety.{name}')".encode() not in guard, name
        assert f"target_safety.{name}".encode() not in guard, name
    for name in ("resource", "bootstrap", "collector", "coverage", "sizing", "kernel"):
        assert f"types.ModuleType('target_safety.{name}')".encode() in guard, name
    supervisor = stage1.sizing_bundle()
    assert b"_S.main(mode='topology-sizing')" in supervisor
    assert stage1.sizing_bundle() != stage1.supervisor_bundle()
    assert b"target_safety.probes.main()" in stage1.supervisor_bundle()
    print("PASS sizing guard payload structurally omits inventory/probes; sizing bundle is distinct")


def main() -> int:
    sizing_guard_payload_carries_no_inventory_or_probes()
    sizing_bundle_succeeds_with_compact_counters()
    sizing_bundle_cap_failure_blocks()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
