# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_blockers.py
"""Behavior regressions for independent acquisition gate B1-B7; local only."""
from __future__ import annotations

import dataclasses
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import target_safety.acquire as acquire
import target_safety.acquire_input_select as selector
import target_safety.acquire_transport as transport
from target_safety.acquire_models import ProcessObservation
from target_safety.acquire_readers import assess_quiet
from acquire_route import _fixture, _ident, _pkg
from acquire_transport import _fixture as transport_fixture
from acquire_fixture_transport import fixture_transport

CLEAN = ProcessObservation((("1", "init", "/"),), None)


def test_cli_when_explicit_modes() -> None:
    # Given the exact driver surface, When dry-run runs, Then no transport is needed.
    driver = Path(transport.__file__)
    result = subprocess.run([sys.executable, "-B", str(driver), "--transport", "ssh",
                             "--dry-run"], capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout)["target_contact"].startswith("none")
    assert transport.parse(["--transport", "ssh", "--execute"]).execute
    both = subprocess.run([sys.executable, "-B", str(driver), "--execute", "--dry-run"],
                          capture_output=True, timeout=10)
    assert both.returncode == 2


def test_selector_when_real_dpkg_directories_and_ld_comments() -> None:
    # Given actual host package listings, When selecting, Then only files/links survive.
    base = Path(tempfile.mkdtemp(prefix="acq-select-", dir="/tmp/opencode"))
    policy = base / "policy.json"
    policy.write_text(json.dumps({"schema": "aa-acquire-input-policy/1",
        "package_rules": ["libc6:amd64", "libc6-dev:amd64", "libstdc++-13-dev:amd64"], "header_roots": ["/usr/include"],
        "lib_dirs": ["/usr/lib", "/lib"], "linker_script_markers": ["GROUP(", "INPUT("]}))
    observed = selector.select_inputs(policy)
    assert observed.specs
    assert all(Path(s.path).is_file() or Path(s.path).is_symlink() for s in observed.specs)
    assert any("/c++/" in s.path for s in observed.specs)
    refs = selector._resolve_linker('/* GNU ld script /bogus */\nGROUP ( /lib/a.so '
                                    'AS_NEEDED ( /lib/b.so ) ) INPUT ( "/lib/with space.so" )')
    assert refs == {"/lib/a.so", "/lib/b.so", "/lib/with space.so"}, refs


def test_window_when_busy_or_missing() -> None:
    base = Path(tempfile.mkdtemp(prefix="acq-window-", dir="/tmp/opencode"))
    roots, links, inputs = _fixture(base)
    request = acquire.WindowRequest(roots, links, inputs, base / "scratch", "blocked",
                                    "jetson.local", _pkg, _ident)
    busy = ProcessObservation((("9", "editor", str(roots[0])),), None)
    # When a relevant writer is present, Then copy never starts and the gate fails.
    with patch.object(acquire, "observe_processes", return_value=busy):
        result = acquire.run_window(request)
    assert result.acquire_error is not None
    assert not result.record.entries
    assert acquire.exit_code(result.diff.verdict, result.acquire_error) != 0
    # Given a missing input, When acquisition fails, Then after evidence is retained.
    missing = dataclasses.replace(request, scratch=base / "missing",
        input_set=(acquire.InputSpec(str(base / "absent")),))
    with patch.object(acquire, "observe_processes", return_value=CLEAN):
        result = acquire.run_window(missing)
    assert result.acquire_error is not None and result.after_manifest.exists()
    assert result.receipt.exists()
    assert not assess_quiet(ProcessObservation((("2", "root", None),), None), CLEAN,
                            roots).proven


def test_transport_when_backpressure_or_timeout() -> None:
    base = Path(tempfile.mkdtemp(prefix="acq-pipes-", dir="/tmp/opencode"))
    # Given stderr larger than a pipe, When both streams drain, Then completion is finite.
    argv = (sys.executable, "-c", "import sys;sys.stderr.write('e'*200000);sys.stdout.write('ok')")
    result = transport._run_bounded(argv, b"", base / "out", 100, 2)
    assert result.exit_status == 0 and (base / "out").read_bytes() == b"ok"
    # When the child never completes, Then it is reaped with a timeout outcome.
    result = transport._run_bounded((sys.executable, "-c", "import signal;signal.pause()"),
                                    b"", base / "timeout", 100, 1)
    assert result.timed_out and result.exit_status != 0


def test_return_when_corrupted_or_failed() -> None:
    base = Path(tempfile.mkdtemp(prefix="acq-return-", dir="/tmp/opencode"))
    cfg, _ = transport_fixture(base)
    # When the actual payload/archive route returns, Then content is verified.
    with fixture_transport(base):
        result = transport.run_window(cfg, "local")
    assert result["success"], result
    returned = Path(result["returned"])
    entry = json.loads((returned / "acquisition.json").read_text())["entries"][0]
    (returned / "snapshot" / entry["copied_path"]).write_bytes(b"CORRUPTED")
    try:
        transport.verify_returned(returned, cfg)
    except (transport.Blocked, OSError):
        pass
    else:
        raise AssertionError("corrupt returned input accepted")


def main() -> int:
    failures = []
    for test in (test_cli_when_explicit_modes, test_selector_when_real_dpkg_directories_and_ld_comments,
                 test_window_when_busy_or_missing, test_transport_when_backpressure_or_timeout,
                 test_return_when_corrupted_or_failed):
        try:
            test()
        except (AssertionError, SystemExit, OSError, KeyError, transport.Blocked, subprocess.TimeoutExpired) as error:
            failures.append(test.__name__)
            print("FAIL", test.__name__, repr(error))
        else:
            print("PASS", test.__name__)
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
