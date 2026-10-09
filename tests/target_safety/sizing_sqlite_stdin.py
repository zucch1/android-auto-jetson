# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_sqlite_stdin.py
"""Real stdin bundle QA for the active SQLite watched-absence contract.

The real sizing and probes bundles run from stdin over a private fixture with
the exact fixed policy substituted: sizing must pass with the typed record,
fail closed when the contract is broken, and probes must reject at its final
boundary after the second inventory and Git observations. No SSH, no target.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage
from target_safety.payload import bundle, sizing_bundle, sizing_guard_bundle, supervisor_bundle
from target_safety.report import accepted_sizing_output
from ceiling_setting_fixtures import fake_setting
from sizing_sqlite_fixtures import LINK_TEXT, exact_record, topology

PY = (sys.executable, "-B", "-")


def _seams(repo, external, link, target, namespaces, extra="") -> bytes:
    return f"""
import target_safety.sizing as _Z, target_safety.coverage as _C
from pathlib import Path
_Z.REPOSITORY = Path({str(repo)!r}); _Z.EXTERNAL = Path({str(external)!r})
_C.REPOSITORY = _Z.REPOSITORY; _C.EXTERNAL = _Z.EXTERNAL
_C.NAMESPACE_LINKS = (Path({str(namespaces[0])!r}), Path({str(namespaces[1])!r}))
_Z.NAMESPACE_LINKS = _C.NAMESPACE_LINKS
_C.SQLITE_LINK = Path({str(link)!r}); _C.SQLITE_LINK_TEXT = {LINK_TEXT!r}
_C.SQLITE_TARGET = Path({str(target)!r})
{extra}""".encode()


def _sizing_payload(root, repo, external, link, target, namespaces, extra="") -> bytes:
    setting, state = fake_setting(root)
    guard = sizing_guard_bundle().rsplit(b"raise SystemExit", 1)[0]
    fixture_guard = guard + _seams(repo, external, link, target, namespaces, extra) \
        + b"raise SystemExit(target_safety.sizing.main())\n"
    prefix = sizing_bundle().rsplit(b"raise SystemExit", 1)[0]
    return prefix + f"""
from pathlib import Path as _P
import target_safety.coverage as _VC
_VC.REPOSITORY = _P({str(repo)!r}); _VC.EXTERNAL = _P({str(external)!r})
_VC.SQLITE_LINK = _P({str(link)!r}); _VC.SQLITE_LINK_TEXT = {LINK_TEXT!r}
_VC.SQLITE_TARGET = _P({str(target)!r})
from target_safety.setting import SudoSetting
_setting = SudoSetting(sudo={setting.sudo!r}, sysctl={setting.sysctl!r})
_S.GUARD_BUNDLE = {fixture_guard!r}
raise SystemExit(_S.main(setting=_setting, mode='topology-sizing'))
""".encode()


def _probes_payload(root, repo, external, link, target, namespaces, extra="") -> bytes:
    setting, state = fake_setting(root)
    guard = bundle().rsplit(b"raise SystemExit", 1)[0]
    synthetic = f"""
import target_safety.probes as _P
_P.REPOSITORY = Path({str(repo)!r}); _P.EXTERNAL = Path({str(external)!r})
_P.NAMESPACE_LINKS = _C.NAMESPACE_LINKS
def _fixture_git(watch):
    watch.check()
    return ('fixture-status', 'fixture-tracked', 'fixture-ignored')
def _fixture_prerequisites(watch):
    watch.check()
_P.git_inventory = _fixture_git
_P.prerequisites = _fixture_prerequisites
"""
    fixture_guard = guard + _seams(repo, external, link, target, namespaces, synthetic + extra) \
        + b"raise SystemExit(target_safety.probes.main())\n"
    prefix = supervisor_bundle().rsplit(b"raise SystemExit", 1)[0]
    return prefix + f"""
from target_safety.setting import SudoSetting
_setting = SudoSetting(sudo={setting.sudo!r}, sysctl={setting.sysctl!r})
_S.GUARD_BUNDLE = {fixture_guard!r}
raise SystemExit(_S.main(setting=_setting))
""".encode()


def sizing_bundle_active_contract_passes() -> None:
    # Given: the real sizing bundle over the exact fixture. Then: the run is
    # accepted and the completion carries the exact non-null record.
    repo, external, link, target, namespaces = topology("stdin-pass")
    root = link.parents[8]
    result = subprocess.run(PY, input=_sizing_payload(root, repo, external, link, target, namespaces),
                            capture_output=True, timeout=90, check=False)
    stdout = result.stdout.decode()
    assert result.returncode == 0, result.stderr or stdout
    with patch.multiple(coverage, REPOSITORY=repo, EXTERNAL=external, SQLITE_LINK=link,
                        SQLITE_LINK_TEXT=LINK_TEXT, SQLITE_TARGET=target):
        assert accepted_sizing_output(stdout), stdout
    outcome = next(json.loads(line)["data"] for line in stdout.splitlines()
                   if json.loads(line).get("event") == "sizing_outcome")
    assert outcome["sizing"]["sqlite_absence"] == exact_record(link, target)
    assert outcome["sizing"]["roots"] == [str(repo), str(external)]
    print(f"PASS active-contract sizing bundle passes with the exact record; fixture={root}")


def sizing_bundle_broken_contract_blocks() -> None:
    # Given: the same bundle with the target present. Then: the guard blocks,
    # no completion exists and acceptance rejects.
    repo, external, link, target, namespaces = topology("stdin-fail")
    target.write_bytes(b"appeared")
    root = link.parents[8]
    result = subprocess.run(PY, input=_sizing_payload(root, repo, external, link, target, namespaces),
                            capture_output=True, timeout=90, check=False)
    stdout = result.stdout.decode()
    assert result.returncode == 70, stdout
    assert "guard_blocked" in stdout and "sqlite_target_present" in stdout, stdout
    assert "topology_sizing_complete" not in stdout
    assert not accepted_sizing_output(stdout)
    print(f"PASS active-contract sizing bundle blocks a present target; fixture={root}")


def probes_bundle_records_the_same_evidence() -> None:
    # Given: the real probes bundle over the exact fixture. Then: the run
    # passes and coverage_established carries the same typed evidence.
    repo, external, link, target, namespaces = topology("stdin-probes-pass")
    root = link.parents[8]
    result = subprocess.run(PY, input=_probes_payload(root, repo, external, link, target, namespaces),
                            capture_output=True, timeout=90, check=False)
    stdout = result.stdout.decode()
    assert result.returncode == 0, result.stderr or stdout
    evidence = None
    for line in stdout.splitlines():
        node = json.loads(line)
        data = node.get("data")
        if node.get("event") == "guard_event" and isinstance(data, str):
            inner = json.loads(data)
            if inner.get("event") == "coverage_established":
                evidence = json.loads(inner["data"])
    assert evidence is not None, stdout
    assert evidence["sqlite_absence"] == exact_record(link, target)
    print(f"PASS probes bundle records the exact evidence; fixture={root}")


def probes_bundle_rejects_at_the_final_boundary() -> None:
    # Given: the target created deterministically after the second inventory
    # and Git observations. Then: the final-boundary recheck rejects the run.
    repo, external, link, target, namespaces = topology("stdin-probes-fail")
    root = link.parents[8]
    hook = f"""
import target_safety.probes as _P
_calls = []
_real_git = _P.git_inventory
def _hooked_git(watch):
    result = _real_git(watch)
    _calls.append(1)
    if len(_calls) == 2:
        Path({str(target)!r}).write_bytes(b'appeared')
    return result
_P.git_inventory = _hooked_git
"""
    result = subprocess.run(PY, input=_probes_payload(root, repo, external, link, target,
                                                      namespaces, hook),
                            capture_output=True, timeout=90, check=False)
    stdout = result.stdout.decode()
    assert result.returncode == 70, stdout
    assert "guard_blocked" in stdout, stdout
    assert ("sqlite_target_present" in stdout or "watch_violation" in stdout
            or "window_invalidated" in stdout), stdout
    assert "protected_no_detected_write_no_persistent_change" not in stdout
    print(f"PASS probes bundle rejects at the final boundary after second inventory/Git; fixture={root}")


def main() -> int:
    sizing_bundle_active_contract_passes()
    sizing_bundle_broken_contract_blocks()
    probes_bundle_records_the_same_evidence()
    probes_bundle_rejects_at_the_final_boundary()
    print("PASS all sqlite watched-absence stdin bundle suites")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
