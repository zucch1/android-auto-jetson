# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_production.py
"""Real production guard main/protect/inventory via stdin; memory-only fixture seams.

Setting authority and prerequisite/Git observations are synthetic. Watch sessions,
coverage, namespace-link handling, inventory, final drain and fd close are real.
No actual remote protected path, Git/package command, sudo or sysctl is accessed.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import stage1
from target_safety.report import accepted_ceiling_output
from ceiling_setting_fixtures import fake_setting


def main() -> int:
    # Given: a tiny two-root approved-scope topology, only under a unique tmpdir.
    root = Path(tempfile.mkdtemp(prefix="ceiling-production-", dir="/tmp/opencode"))
    repo, external = root / "repository", root / "external"
    repo.mkdir()
    external.mkdir()
    (repo / "input.txt").write_text("fixture input\n")
    (external / "graph.txt").write_text("fixture graph\n")
    (repo / ".codegraph").symlink_to(external)
    namespaces = (repo / ".local_env/aqt-venv/bin/python3", repo / ".venv/bin/python3")
    for namespace in namespaces:
        namespace.parent.mkdir(parents=True)
        namespace.symlink_to("/usr/bin/python3")
    setting, state = fake_setting(root)
    guard = stage1.bundle()
    guard_prefix = guard.rsplit(b"raise SystemExit", 1)[0]
    seams = f"""
import target_safety.probes as _P, target_safety.coverage as _C
from pathlib import Path
_P.REPOSITORY = Path({str(repo)!r})
_P.EXTERNAL = Path({str(external)!r})
_C.NAMESPACE_LINKS = (Path({str(namespaces[0])!r}), Path({str(namespaces[1])!r}))
_P.NAMESPACE_LINKS = _C.NAMESPACE_LINKS
def _fixture_git(watch):
    watch.check()
    return ('fixture-status', 'fixture-tracked', 'fixture-ignored')
def _fixture_prerequisites(watch):
    watch.check()
_P.git_inventory = _fixture_git
_P.prerequisites = _fixture_prerequisites
""".encode()
    fixture_guard = guard_prefix + seams + b"raise SystemExit(target_safety.probes.main())\n"
    production = stage1.supervisor_bundle()
    prefix = production.rsplit(b"raise SystemExit", 1)[0]
    injection = f"""
from target_safety.setting import SudoSetting
_setting = SudoSetting(sudo={setting.sudo!r}, sysctl={setting.sysctl!r})
_S.GUARD_BUNDLE = {fixture_guard!r}
raise SystemExit(_S.main(setting=_setting))
""".encode()
    payload = prefix + injection
    (root / "fixture-payload.py").write_bytes(payload)
    # When: real default python -B - supervisor AND production guard main.
    result = subprocess.run((sys.executable, "-B", "-"), input=payload,
                            capture_output=True, timeout=20, check=False)
    (root / "stdout.txt").write_bytes(result.stdout)
    (root / "stderr.txt").write_bytes(result.stderr)
    # Then: a real final guard event after inventories/watch closure, plus restore.
    assert result.returncode == 0, result.stderr or result.stdout
    assert accepted_ceiling_output(result.stdout.decode()), result.stdout
    assert state.read_text() == "65536"
    outcome = next(json.loads(line)["data"] for line in result.stdout.decode().splitlines()
                   if json.loads(line)["event"] == "ceiling_outcome")
    events = [event["event"] for event in outcome["guard_events"]]
    assert "coverage_established" in events and "protected_before" in events and "protected_after" in events
    assert events[-2:] == ["watch_closed", "protected_no_detected_write_no_persistent_change"]
    coverage_data = json.loads(json.loads(next(event["raw"] for event in outcome["guard_events"]
                                               if event["event"] == "coverage_established"))["data"])
    assert coverage_data["namespace_only"] == [str(namespaces[0]), str(namespaces[1])]
    assert coverage_data["literal_link_text"] == "/usr/bin/python3"
    assert coverage_data["referent_protected"] is False
    print(f"PASS real production stdin guard main/protect/inventory/close on tiny fixture; artifacts={root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
