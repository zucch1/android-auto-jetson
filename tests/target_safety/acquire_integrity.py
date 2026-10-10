# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_integrity.py
"""Acquisition gate regressions: live selector route, failures, and return integrity."""
from __future__ import annotations

import dataclasses
import hashlib
import io
import json
import os
import signal
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from acquire_fixture_transport import fixture_transport
from acquire_transport import _fixture
import target_safety.acquire_transport as transport
import target_safety.acquire as acquire
from target_safety.acquire_models import InputObservation, ProcessObservation
from target_safety.acquire_verify import extract_returned
from acquire_route import _fixture as window_fixture, _ident, _pkg

CLEAN = ProcessObservation((("1", "fixture-init", "/"),), None)


def test_live_selector_when_real_payload_returns() -> None:
    # Given a representative C++/libc package tree, When the live selector runs, Then closure returns intact.
    base = Path(tempfile.mkdtemp(prefix="acq-live-", dir="/tmp/opencode"))
    cfg, tgt = _fixture(base)
    (tgt / "usr/include/c++/13/bits").mkdir(parents=True)
    (tgt / "usr/include/c++/13/vector").write_text("C++ vector header")
    (tgt / "usr/include/c++/13/bits/c++config.h").write_text("C++ architecture header")
    (tgt / "usr/lib/libc.so").write_text('/* GNU ld script /not-a-path */\nGROUP ( libf.so '
                                        'AS_NEEDED ( "lib with space.so" ) )')
    (tgt / "usr/lib/lib with space.so").write_bytes(b"library")
    os.symlink(str(tgt / "usr/lib/libf.so.1"), tgt / "usr/lib/absolute.so")
    cfg = dataclasses.replace(cfg, observation=None, fixture=True)
    with fixture_transport(base, live=True):
        result = transport.run_window(cfg, "local")
    assert result["success"], result
    returned = Path(result["returned"])
    record = json.loads((returned / "acquisition.json").read_bytes())
    assert record["provenance"] == "fixture"
    assert len(record["entries"]) == 8, record["entries"]
    assert result["verification"]["entries_verified"] == 8


def test_driver_when_explicit_execute_local() -> None:
    # Given the real CLI with fake local readers, When --execute runs, Then the full result succeeds.
    base = Path(tempfile.mkdtemp(prefix="acq-cli-", dir="/tmp/opencode"))
    cfg, _ = _fixture(base)
    observation = base / "observation.json"
    observation.write_bytes(cfg.observation)
    argv = [sys.executable, "-B", transport.__file__, "--execute", "--transport", "local",
            "--input-set", str(observation), "--remote-scratch", cfg.remote_scratch,
            "--local-out", str(cfg.local_out), "--window-id", cfg.window_id,
            *[v for r in cfg.roots for v in ("--root", r)],
            *[v for e in cfg.link_exceptions for v in ("--link-exception", e)]]
    with fixture_transport(base):
        result = subprocess.run(argv, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr + result.stdout
    assert json.loads(result.stdout)["success"]


def test_reject_when_returned_content_changes() -> None:
    base = Path(tempfile.mkdtemp(prefix="acq-integrity-", dir="/tmp/opencode"))
    cfg, _ = _fixture(base)
    with fixture_transport(base):
        result = transport.run_window(cfg, "local")
    assert result["success"], result
    returned = Path(result["returned"])
    record = json.loads((returned / "acquisition.json").read_bytes())
    entry = next(e for e in record["entries"] if e["kind"] == "regular")
    leaf = returned / "snapshot" / entry["copied_path"]
    original = leaf.read_bytes()
    mutations = (lambda: leaf.write_bytes(b"corrupt"), lambda: leaf.unlink(),
                 lambda: (returned / "snapshot/extra").write_bytes(b"extra"))
    for mutate in mutations:
        # When returned leaves are corrupt/missing/extra, Then verification refuses them.
        mutate()
        try:
            transport.verify_returned(returned, cfg)
        except (transport.Blocked, OSError):
            pass
        else:
            raise AssertionError("changed tree accepted")
        if not leaf.exists() or leaf.read_bytes() != original:
            leaf.write_bytes(original)
        (returned / "snapshot/extra").unlink(missing_ok=True)
    # When a record changes without its receipt binding, Then it is rejected.
    path = returned / "acquisition.json"
    saved = path.read_bytes()
    path.write_bytes(saved + b" ")
    try:
        transport.verify_returned(returned, cfg)
    except transport.Blocked as error:
        assert error.reason == "receipt_hash_mismatch"
    else:
        raise AssertionError("unbound record accepted")
    path.write_bytes(saved)
    for changed in (dataclasses.replace(cfg, window_id="other"),
                    dataclasses.replace(cfg, roots=("/different",))):
        try:
            transport.verify_returned(returned, changed)
        except transport.Blocked:
            pass
        else:
            raise AssertionError("wrong window/roots accepted")
    # Given a re-bound but incomplete manifest, When validated, Then consumer still rejects.
    path = returned / "after.json"
    raw = json.loads(path.read_bytes())
    raw["trailer"]["complete"] = False
    path.write_text(json.dumps(raw))
    receipt_path = returned / "receipt.json"
    receipt = json.loads(receipt_path.read_bytes())
    receipt["after_manifest"]["sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
    receipt_path.write_text(json.dumps(receipt))
    try:
        transport.verify_returned(returned, cfg)
    except transport.Blocked as error:
        assert error.reason == "manifest_incomplete"
    else:
        raise AssertionError("incomplete manifest accepted")


def test_failure_when_transport_acquisition_or_archive_fails() -> None:
    for failure in ("acquisition", "archive", "transport", "remote-timeout", "local-verification-timeout"):
        # Given one failing lifecycle phase, When run once, Then durable outcome is failure.
        base = Path(tempfile.mkdtemp(prefix="acq-fail-", dir="/tmp/opencode"))
        cfg, tgt = _fixture(base)
        budget = transport.load_budget()
        if failure == "acquisition":
            (tgt / "usr/include/h.h").unlink()
        with fixture_transport(base), patch.object(transport, "load_budget", return_value=budget):
            if failure == "archive":
                command = transport.remote_command(cfg).replace("before.json after.json", "missing.json after.json")
                with patch.object(transport, "remote_command", return_value=command):
                    result = transport.run_window(cfg, "local")
            elif failure == "remote-timeout":
                budget["remote_deadline_seconds"] = 1
                command = transport.remote_command(cfg).replace("set +e", "sleep 3\nset +e")
                with patch.object(transport, "remote_command", return_value=command):
                    result = transport.run_window(cfg, "local")
                assert result["receipt"]["timed_out"]
            elif failure == "local-verification-timeout":
                budget["total_budget_seconds"] = 1
                with patch.object(transport, "verify_returned", side_effect=lambda *args: signal.pause()):
                    result = transport.run_window(cfg, "local")
                assert result["timed_out"]
            elif failure == "transport":
                original = transport._run_bounded
                def failed_transport(*args):
                    return dataclasses.replace(original(*args), exit_status=70)
                with patch.object(transport, "_run_bounded", side_effect=failed_transport):
                    result = transport.run_window(cfg, "local")
                assert result["verification"]["equivalent"]
            else:
                result = transport.run_window(cfg, "local")
                assert result["receipt"]["exit_status"] == 70
                assert (cfg.local_out / "returned/after.json").exists()
        assert not result["success"], result
        assert json.loads((cfg.local_out / "transport-result.json").read_bytes())["success"] is False


def test_window_when_each_quiet_boundary_is_busy() -> None:
    for index in range(4):
        # Given activity at any required sample, When the window runs, Then it fails.
        base = Path(tempfile.mkdtemp(prefix="acq-quiet-", dir="/tmp/opencode"))
        roots, links, specs = window_fixture(base)
        request = acquire.WindowRequest(roots, links, specs, base / "scratch", "quiet",
                                        "jetson.local", _pkg, _ident)
        samples = [CLEAN] * 4
        samples[index] = ProcessObservation((("9", "editor", str(roots[0])),), None)
        with patch.object(acquire, "observe_processes", side_effect=samples):
            result = acquire.run_window(request)
        assert result.acquire_error is not None and not result.record.completed
        if index < 3:
            assert not result.record.entries
    assert not acquire.assess_quiet(dataclasses.replace(CLEAN, complete=False), CLEAN, roots).proven


def test_process_sample_when_truncated_or_inaccessible() -> None:
    # Given ordinary sampling limits/access failure, When sampled, Then quiet is unproven.
    from acquire_proc import test_sample_when_limit_or_access_unknown
    test_sample_when_limit_or_access_unknown()


def test_selection_when_bracketed_read_fails() -> None:
    base = Path(tempfile.mkdtemp(prefix="acq-bracket-", dir="/tmp/opencode"))
    roots, links, specs = window_fixture(base)
    scratch = base / "scratch"
    def select():
        assert (scratch / "before.json").exists()
        assert not (scratch / "after.json").exists()
        raise OSError("ordinary selection read failure")
    request = acquire.WindowRequest(roots, links, (), scratch, "selection", "jetson.local",
                                    _pkg, _ident, select=select)
    # When a real selection callback reads inside the window and fails, Then after is retained.
    with patch.object(acquire, "observe_processes", return_value=CLEAN):
        result = acquire.run_window(request)
    assert result.acquire_error.reason == "acquisition_read_failed"
    assert result.after_manifest.exists() and result.receipt.exists()


def test_archive_when_link_escapes() -> None:
    base = Path(tempfile.mkdtemp(prefix="acq-tar-", dir="/tmp/opencode"))
    archive = base / "escape.tar"
    with tarfile.open(archive, "w") as tar:
        entry = tarfile.TarInfo("snapshot/escape")
        entry.type = tarfile.SYMTYPE
        entry.linkname = "/tmp/outside"
        tar.addfile(entry)
    # When a returned link escapes, Then extraction rejects it before following it.
    try:
        extract_returned(archive, base / "returned", 10240)
    except tarfile.FilterError:
        pass
    else:
        raise AssertionError("escaping link accepted")


if __name__ == "__main__":
    for test in (test_live_selector_when_real_payload_returns, test_reject_when_returned_content_changes,
                 test_driver_when_explicit_execute_local,
                 test_failure_when_transport_acquisition_or_archive_fails,
                 test_window_when_each_quiet_boundary_is_busy, test_selection_when_bracketed_read_fails,
                 test_process_sample_when_truncated_or_inaccessible,
                 test_archive_when_link_escapes):
        test()
        print("PASS", test.__name__)
