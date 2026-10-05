# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_budget.py
"""Real setting timeout and synthetic worst-case cleanup/policy qualification."""
from __future__ import annotations

import io
import tempfile
import sys
import time
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.guard import GuardChild, GuardResult, GuardStateError
from target_safety.policy import require_passwordless
from target_safety.setting import CEILING_ORIGINAL, CEILING_TEMPORARY, SettingError, SudoSetting
from target_safety.supervisor import Supervisor
from ceiling_window import FakeSetting
from ceiling_setting_fixtures import fake_setting

COMMANDS = ("/usr/sbin/sysctl -w fs.inotify.max_user_watches=65536",
            "/usr/sbin/sysctl -w fs.inotify.max_user_watches=1048576")
POLICY = "Sudoers entry:\n    RunAsUsers: root\n    Options: !authenticate\n    Commands:\n        " + "\n        ".join(COMMANDS)


def policy_only_accepts_exact_root_passwordless_pair() -> None:
    # Given / When / Then: strict long-list policy, not exit status or cached credentials.
    require_passwordless(POLICY, COMMANDS)
    variants = (POLICY.replace("!authenticate", "authenticate"),
                POLICY.replace("root", "ALL"), POLICY.replace("root", "daemon"),
                POLICY.replace("65536", "*"), POLICY.replace("/usr/sbin/sysctl", "/sbin/sysctl"),
                POLICY + "\n" + POLICY, POLICY + "\n        ALL",
                "Matching Defaults entries for user on host:\n    !authenticate\n" + POLICY,
                POLICY.replace("1048576", "65536"), POLICY.replace("!authenticate", "!authenticate, noexec"))
    for policy in variants:
        try:
            require_passwordless(policy, COMMANDS)
        except SettingError as error:
            assert error.operation == "preflight_passwordless_unproven"
        else:
            raise AssertionError(f"unsupported policy accepted: {policy}")


def setting_timeout_includes_failure_cleanup() -> None:
    # Given: a real local Python process, no actual sudo/sysctl command.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as root:
        setting, _ = fake_setting(Path(root))
        setting = SudoSetting(sudo=setting.sudo, sysctl=setting.sysctl, timeout_seconds=10.0)
        started = time.monotonic()
        # When / Then
        with patch.dict("os.environ", {"AA_FAKE_READ": "hang"}):
            try:
                setting.read(timeout=20.0)
            except SettingError as error:
                assert error.operation == "timeout", error
            else:
                raise AssertionError("hanging setting accepted")
        elapsed = time.monotonic() - started
    assert elapsed < 5.3, elapsed
    print(f"PASS real setting process timeout/reap within five-second total allowance; elapsed={elapsed:.3f}s")


def cleanup_budget_reserves_restore_and_egress() -> None:
    # Given: synthetic clock charges 900 child + 10 stop + 5 collection, then 3*5 restore.
    clock = [0.0]
    restore_bounds: list[float] = []

    class ChargedSetting(FakeSetting):
        def read(self, timeout: float | None = None, *, deadline: float | None = None) -> int:
            if clock[0] >= 915.0:
                assert timeout is not None
                restore_bounds.append(timeout)
                clock[0] += timeout
            return super().read(timeout)

        def write(self, value: int, timeout: float | None = None, *, deadline: float | None = None) -> None:
            if value == CEILING_ORIGINAL:
                assert timeout is not None
                restore_bounds.append(timeout)
                clock[0] += timeout
            super().write(value, timeout)

    setting = ChargedSetting(CEILING_ORIGINAL)

    def wait(_guard: GuardChild) -> GuardResult:
        clock[0] = 915.0
        return GuardResult(124, "", "", True, True)

    with patch("target_safety.supervisor.time.monotonic", side_effect=lambda: clock[0]), \
            patch.object(GuardChild, "spawn"), patch.object(GuardChild, "wait", wait), \
            patch.object(GuardChild, "terminate"), redirect_stdout(io.StringIO()):
        # When
        outcome = Supervisor(setting, (sys.executable, "-c", "pass")).run()
    # Then
    assert outcome.restored_original and not outcome.accepted
    assert restore_bounds == [5.0, 5.0, 5.0] and clock[0] == 930.0


def exhausted_cleanup_is_explicit_rejection() -> None:
    # Given: a synthetic lifecycle fault exhausts the total deadline.
    clock = [0.0]
    setting = FakeSetting(CEILING_ORIGINAL)

    def wait(_guard: GuardChild) -> GuardResult:
        clock[0] = 941.0
        raise GuardStateError("synthetic exhausted lifecycle")

    with patch("target_safety.supervisor.time.monotonic", side_effect=lambda: clock[0]), \
            patch.object(GuardChild, "spawn"), patch.object(GuardChild, "wait", wait), \
            patch.object(GuardChild, "terminate"), redirect_stdout(io.StringIO()):
        # When
        outcome = Supervisor(setting, (sys.executable, "-c", "pass")).run()
    # Then: no new operation can start after its allowance is exhausted; do not invent restoration.
    assert not outcome.restored_original and not outcome.accepted
    assert any("restore_budget_exhausted" in error for error in outcome.errors)
    assert "egress_failed_or_budget_exhausted" in outcome.errors
    assert setting.value == CEILING_TEMPORARY


def missing_guard_executable_is_typed() -> None:
    # Given / When / Then: actual launch failure remains inside the guard boundary.
    guard = GuardChild(("/nonexistent/aa-ceiling-guard",), 1.0)
    try:
        guard.spawn()
    except GuardStateError as error:
        assert "spawn" in error.state
    else:
        raise AssertionError("missing guard accepted")


def main() -> int:
    for test in (policy_only_accepts_exact_root_passwordless_pair,
                 setting_timeout_includes_failure_cleanup, cleanup_budget_reserves_restore_and_egress,
                 exhausted_cleanup_is_explicit_rejection, missing_guard_executable_is_typed):
        test()
        print(f"PASS {test.__name__}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
