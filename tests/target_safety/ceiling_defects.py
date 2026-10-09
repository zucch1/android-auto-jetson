# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_defects.py
"""Oracle regressions: real local pipes/processes, synthetic authority/clock faults."""
from __future__ import annotations

import json
import os
import signal
import subprocess
import sys
import tempfile
import traceback
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import stage1
from target_safety.guard import GuardChild
from target_safety.runner import RunResult
from target_safety.setting import CEILING_ORIGINAL, SettingError, SudoSetting
from target_safety.supervisor import Supervisor
from ceiling_fixtures import guard_argv
from ceiling_window import FakeSetting


def termination_error_still_restores() -> None:
    # Given: a completed real child, but a failing lifecycle boundary.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as root:
        setting = FakeSetting(CEILING_ORIGINAL)
        previous = signal.getsignal(signal.SIGTERM)
        with patch.object(GuardChild, "terminate", side_effect=OSError("termination denied")):
            # When
            outcome = Supervisor(setting, guard_argv(Path(root), "success")).run()
        # Then
        assert setting.value == CEILING_ORIGINAL and outcome.restored_original
        assert not outcome.accepted and any("termination" in e for e in outcome.errors)
        assert signal.getsignal(signal.SIGTERM) == previous


def missing_setting_executable_is_typed() -> None:
    # Given: a definitely absent executable (never sudo/sysctl).
    setting = SudoSetting(sysctl="/nonexistent/aa-ceiling-executable")
    # When / Then
    try:
        setting.read()
    except SettingError as error:
        assert error.operation == "launch_io"
    else:
        raise AssertionError("missing executable accepted")


def policy_rejects_password_cache_and_ambiguity() -> None:
    # Given: permission query exit 0, without strict passwordless evidence.
    outputs = ("", "/usr/sbin/sysctl -w fs.inotify.max_user_watches=65536\n",
               "(root) PASSWD: ALL\n", "(root) NOPASSWD: ALL\n",
               "Sudoers entry:\n    RunAsUsers: root\n    Options: authenticate\n    Commands:\n        ALL\n")
    for output in outputs:
        setting = SudoSetting()
        with patch.object(SudoSetting, "_run", return_value=subprocess.CompletedProcess([], 0, output, "")):
            # When / Then
            try:
                setting.preflight()
            except SettingError:
                continue
            raise AssertionError(f"unsupported/password-required policy accepted: {output!r}")


def total_deadline_reaches_setting_operations() -> None:
    # Given: a clock advanced by each operation, never wall-clock sleeping.
    clock = [100.0]
    bounds: list[float | None] = []
    deadlines: list[float | None] = []

    class BudgetSetting(FakeSetting):
        def read(self, timeout: float | None = None, *, deadline: float | None = None) -> int:
            bounds.append(timeout)
            deadlines.append(deadline)
            return super().read()

        def preflight(self, timeout: float | None = None, *, deadline: float | None = None) -> None:
            bounds.append(timeout)
            deadlines.append(deadline)
            clock[0] += 2.0

        def write(self, value: int, timeout: float | None = None, *, deadline: float | None = None) -> None:
            bounds.append(timeout)
            super().write(value)
            clock[0] += 1.0

    setting = BudgetSetting(CEILING_ORIGINAL)
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as root, \
            patch("target_safety.supervisor.time.monotonic", side_effect=lambda: clock[0]):
        # When
        outcome = Supervisor(setting, guard_argv(Path(root), "success"),
                             budget_seconds=3.0, cleanup_headroom_seconds=1.0).run()
    # Then: expired setup must not perform the raise or start a guard.
    assert not outcome.accepted and outcome.guard_exit is None
    assert not setting.writes
    assert all(bound is not None and 0 < bound <= 5.0 for bound in bounds)
    assert deadlines == [102.0, 102.0, 102.75, 102.75]
    assert any("budget" in error for error in outcome.errors)


def full_stdout_pipe_restores_before_delivery() -> None:
    # Given: a real nonreading stdout pipe filled only AFTER the fake raise.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as root:
        state = Path(root) / "state"
        state.write_text(str(CEILING_ORIGINAL))
        report = Path(root) / "outcome.json"
        script = f"""
import json, os, sys
from dataclasses import asdict
from pathlib import Path
sys.path[:0] = {[str(stage1.WORKTREE / 'tools'), str(stage1.WORKTREE / 'tests/target_safety')]!r}
from ceiling_window import FakeSetting
from target_safety.supervisor import Supervisor
class Setting(FakeSetting):
    def write(self, value, timeout=None, *, deadline=None):
        super().write(value)
        Path({str(state)!r}).write_text(str(value))
        if value == 1048576:
            os.set_blocking(1, False)
            try:
                while True: os.write(1, b'x' * 4096)
            except BlockingIOError:
                os.set_blocking(1, True)
s = Setting(65536)
guard = "import json; print(json.dumps(dict(event='protected_no_detected_write_no_persistent_change', data='ok')))"
out = Supervisor(s, (sys.executable, '-c', guard),
                 budget_seconds=1.0, cleanup_headroom_seconds=0.5).run()
Path({str(report)!r}).write_text(json.dumps(asdict(out)))
raise SystemExit(0 if out.accepted else 70)
"""
        read_fd, write_fd = os.pipe()
        try:
            # When: keep the reader open but NEVER drain it.
            proc = subprocess.Popen((sys.executable, "-B", "-c", script), stdout=write_fd,
                                    stderr=subprocess.PIPE)
            try:
                _, err = proc.communicate(timeout=3.0)
            except subprocess.TimeoutExpired:
                proc.kill()
                _, err = proc.communicate(timeout=2.0)
                raise AssertionError(f"full stdout prevented bounded cleanup; state={state.read_text()}; {err!r}")
            # Then
            assert state.read_text() == str(CEILING_ORIGINAL), err
            assert proc.returncode == 70, err
            outcome = json.loads(report.read_text())
            assert outcome["guard_valid"] and outcome["restored_original"] and not outcome["accepted"]
            assert "egress_failed_or_budget_exhausted" in outcome["errors"]
        finally:
            os.close(write_fd)
            os.close(read_fd)


def remote_success_requires_one_restored_outcome() -> None:
    # Given: only synthetic transport; no real target destination or transport.
    valid = {"event": "ceiling_outcome", "monotonic_ns": 1, "data": {
        "accepted": True, "guard_valid": True, "restored_original": True,
        "original_observed": 65536, "temp_observed": 1048576, "restore_observed": 65536,
        "guard_exit": 0, "errors": [], "conflicts": [], "signal": None, "mode": "ceiling"}}
    raw = json.dumps(valid)
    cases = [("", False), ("not json ceiling_outcome", False),
             ('{"event":"ceiling_outcome","data":', False),
             (json.dumps({"event": "guard_event", "data": raw}), False),
             (raw + "\n" + raw, False), (raw, True)]
    for field, value in (("accepted", 1), ("guard_valid", False), ("restored_original", False),
                         ("restore_observed", 1048576), ("errors", ["fault"]),
                         ("conflicts", ["conflict"]), ("signal", 15), ("guard_exit", 3)):
        altered = json.loads(raw)
        altered["data"][field] = value
        cases.append((json.dumps(altered), False))
    for stdout, accepted in cases:
        for timed_out in (False, True):
            with tempfile.TemporaryDirectory(dir="/tmp/opencode") as root:
                destination = Path(root) / "synthetic-attempt.json"
                qualification = stage1.Receipt(argv=[], command="fake", cwd=root,
                                               exit_status=0, stdout="", stderr="")
                remote = RunResult(argv=(), exit_status=0, stdout=stdout, stderr="", timed_out=timed_out,
                                evidence_complete=True)
                with patch.object(stage1, "run", return_value=qualification) as qualify, \
                        patch.object(stage1, "run_owned", return_value=remote) as transport, ExitStack() as stack:
                    # When
                    code = stage1.attempt_target(stack, destination)
                    assert all(Path(path).is_file() for path in transport.call_args.kwargs['capture'].paths.values())
                # Then
                assert (code == 0) == (accepted and not timed_out), (stdout, timed_out, code)
                assert "ceiling_qa.py" in " ".join(qualify.call_args.args[0])


def main() -> int:
    failures = 0
    for test in (termination_error_still_restores, missing_setting_executable_is_typed,
                 policy_rejects_password_cache_and_ambiguity, total_deadline_reaches_setting_operations,
                 full_stdout_pipe_restores_before_delivery, remote_success_requires_one_restored_outcome):
        try:
            test()
        except (AssertionError, OSError, TypeError) as error:
            failures += 1
            print(f"FAIL {test.__name__}: {error}")
            traceback.print_exc()
        else:
            print(f"PASS {test.__name__}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
