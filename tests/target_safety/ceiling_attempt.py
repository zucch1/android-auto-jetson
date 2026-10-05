# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_attempt.py
"""attempt_target integration: qualification, exclusive reserve, strict SSH transport.

Fakes only the process boundary (qualification run + SSH transport). The production
supervisor payload and SSH target authority are asserted unchanged by the seam.
No real SSH, no target contact, no kernel mutation.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import stage1
from target_safety.runner import RunResult
from target_safety.local_capture import LocalCapture


def _ok_run(fixture: Path):
    def run(argv: tuple[str, ...], payload: bytes | None = None) -> stage1.Receipt:
        return stage1.Receipt(argv=list(argv), command="fake", cwd=str(fixture),
                              exit_status=0, stdout="qualification-ok", stderr="")
    return run


def _transport(calls: list, exit_status: int = 0, evidence_complete: bool = True):
    def run_owned(argv: tuple[str, ...], *, capture: LocalCapture, timeout_seconds: float,
                  stdin: bytes | None = None, cwd: Path | None = None) -> RunResult:
        assert all(Path(path).is_file() for path in capture.paths.values())
        calls.append({"argv": argv, "stdin": stdin, "timeout_seconds": timeout_seconds,
                      "capture": capture.paths})
        outcome = {"event": "ceiling_outcome", "monotonic_ns": 1, "data": {
            "accepted": True, "guard_valid": True, "restored_original": True,
            "original_observed": 65536, "temp_observed": 1048576, "restore_observed": 65536,
            "guard_exit": 0, "errors": [], "conflicts": [], "signal": None, "mode": "ceiling"}}
        return RunResult(argv=argv, exit_status=exit_status, stdout=json.dumps(outcome), stderr="",
                          timed_out=False, evidence_complete=evidence_complete)
    return run_owned


def reserves_then_strict_ssh_with_real_payload() -> None:
    """Given a fresh destination, When attempting, Then reserve then call strict SSH."""
    fixture = Path(tempfile.mkdtemp(prefix="ceiling-attempt-", dir="/tmp/opencode"))
    destination = fixture / "task-5-prerequisites-ceiling-attempt.json"
    calls: list = []
    payload = stage1.supervisor_bundle()
    with patch.object(stage1, "run", _ok_run(fixture)), \
            patch.object(stage1, "run_owned", _transport(calls)):
        with ExitStack() as stack:
            code = stage1.attempt_target(stack, destination)
    assert code == 0
    assert len(calls) == 1
    captured = calls[0]
    assert captured["argv"] == stage1.SSH
    assert captured["stdin"] == payload
    assert captured["timeout_seconds"] == stage1.LOCAL_RUN_TIMEOUT_SECONDS
    written = json.loads(destination.read_text())
    assert written["schema"] == "aa-task5-prerequisites-ceiling-attempt-1"
    assert written["remote"]["exit_status"] == 0
    assert written["remote_stdin_sha256"] == hashlib.sha256(payload).hexdigest()
    print("PASS attempt_target reserves exclusively then strict-SSH with the real supervisor payload")


def test_seams_cannot_alter_payload_or_authority() -> None:
    """Given a patched transport, When read, Then payload/authority constants are unchanged."""
    fixture = Path(tempfile.mkdtemp(prefix="ceiling-attempt-seam-", dir="/tmp/opencode"))
    calls: list = []
    with patch.object(stage1, "run_owned", _transport(calls)):
        assert "jetson.local" in " ".join(stage1.SSH)
        assert b"target_safety.supervisor" in stage1.supervisor_bundle()
        assert stage1.SSH == tuple(stage1.SSH)
        assert stage1.EFFECTIVE_CAPS["registered_watches"] == 1000128
    print("PASS test seams cannot alter the production SSH payload or target authority")


def exclusive_reserves_and_preserves_history() -> None:
    """Given an existing destination, When attempting, Then no transport and no overwrite."""
    fixture = Path(tempfile.mkdtemp(prefix="ceiling-attempt-x-", dir="/tmp/opencode"))
    destination = fixture / "task-5-prerequisites-ceiling-attempt.json"
    destination.write_bytes(b"historical")
    calls: list = []
    with patch.object(stage1, "run", _ok_run(fixture)), \
            patch.object(stage1, "run_owned", _transport(calls)):
        try:
            with ExitStack() as stack:
                stage1.attempt_target(stack, destination)
        except FileExistsError:
            assert not calls, "transport ran before reservation"
            assert destination.read_bytes() == b"historical"
        else:
            raise AssertionError("existing destination overwritten")
    print("PASS attempt_target exclusive open-x preserves history and never overwrites")


def qualification_failure_yields_zero_transport() -> None:
    """Given failing qualification, When attempting, Then zero transport calls, no receipt."""
    fixture = Path(tempfile.mkdtemp(prefix="ceiling-attempt-q-", dir="/tmp/opencode"))
    destination = fixture / "task-5-prerequisites-ceiling-attempt.json"
    calls: list = []

    def run(argv: tuple[str, ...], payload: bytes | None = None) -> stage1.Receipt:
        return stage1.Receipt(argv=list(argv), command="fake", cwd=str(fixture),
                              exit_status=1, stdout="", stderr="qual failed")

    with patch.object(stage1, "run", run), \
            patch.object(stage1, "run_owned", _transport(calls)):
        with ExitStack() as stack:
            code = stage1.attempt_target(stack, destination)
    assert code == 1 and not calls
    assert not destination.exists()
    assert not any(fixture.iterdir())
    print("PASS attempt_target qualification failure yields zero transports and no receipt")


def occupied_sidecars_block_transport() -> None:
    # Given: every exclusive sidecar, with a regular or dangling collision.
    for suffix in ('.remote.stdout', '.remote.stderr', '.lifecycle.jsonl'):
        for dangling in (False, True):
            with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory:
                root = Path(directory)
                destination = root / 'attempt.json'
                occupied = destination.with_suffix(suffix)
                if dangling: occupied.symlink_to(root / 'absent')
                else: occupied.write_bytes(b'historical sidecar')
                calls: list = []
                with patch.object(stage1, 'run', _ok_run(root)), patch.object(stage1, 'run_owned', _transport(calls)):
                    # When / Then
                    try:
                        with ExitStack() as stack: stage1.attempt_target(stack, destination)
                    except FileExistsError: assert not calls
                    else: raise AssertionError('occupied sidecar allowed transport')
                assert destination.is_file() and destination.read_bytes() == b''
                assert os.path.lexists(occupied)
                if dangling: assert occupied.is_symlink() and not (root / 'absent').exists()
                else: assert occupied.read_bytes() == b'historical sidecar'
                assert len(list(root.iterdir())) >= 2
    print('PASS all regular/dangling sidecar collisions block before transport and retain reservations')


def failed_evidence_transport_is_rejected() -> None:
    """Given an incomplete-evidence transport, When attempting, Then rejected despite clean stdout."""
    fixture = Path(tempfile.mkdtemp(prefix="ceiling-attempt-evidence-", dir="/tmp/opencode"))
    destination = fixture / "task-5-prerequisites-ceiling-attempt.json"
    calls: list = []
    with patch.object(stage1, "run", _ok_run(fixture)), \
            patch.object(stage1, "run_owned", _transport(calls, evidence_complete=False)):
        with ExitStack() as stack:
            code = stage1.attempt_target(stack, destination)
    # Then: an accepting stdout outcome never lands on incomplete pump evidence.
    assert code == 1
    written = json.loads(destination.read_text())
    assert written["accepted"] is False
    assert written["remote"]["evidence_complete"] is False
    assert written["remote"]["exit_status"] == 0 and written["remote"]["timed_out"] is False
    print("PASS incomplete pump evidence is rejected even with an accepting stdout outcome")


def main() -> int:
    reserves_then_strict_ssh_with_real_payload()
    failed_evidence_transport_is_rejected()
    test_seams_cannot_alter_payload_or_authority()
    exclusive_reserves_and_preserves_history()
    qualification_failure_yields_zero_transport()
    occupied_sidecars_block_transport()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
