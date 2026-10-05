# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_argv.py
"""Exact production argv and real fake-executable compatibility regressions."""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import time
from contextlib import ExitStack
from pathlib import Path
from typing import Literal, assert_never
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import setting, stage1
from target_safety.setting import SettingError, SudoSetting
from ceiling_setting_fixtures import fake_setting

ASSIGNMENTS = tuple(f"fs.inotify.max_user_watches={v}" for v in (65536, 1048576))
POLICY = "Sudoers entry:\nRunAsUsers: root\nOptions: !authenticate\nCommands:\n" + "\n".join(
    f"/usr/sbin/sysctl -w {a}" for a in ASSIGNMENTS)


def exact_production_argv() -> None:
    # Given: a wire seam that never launches actual sudo/sysctl.
    calls: list[tuple[str, ...]] = []
    def execute(argv: tuple[str, ...], deadline: float) -> subprocess.CompletedProcess[str]:
        calls.append(argv)
        return subprocess.CompletedProcess(argv, 0, "65536" if "fs.inotify.max_user_watches" in argv else POLICY, "")
    with patch.object(setting, "execute", execute):
        # When
        boundary = SudoSetting()
        boundary.read()
        boundary.preflight()
        for value in (65536, 1048576): boundary.write(value)
    # Then: complete argv, including both option terminators and no privileged reads.
    expected = [("/usr/sbin/sysctl", "-n", "fs.inotify.max_user_watches")]
    expected.extend(("sudo", "-n", "-u", "root", "-l", "--", "/usr/sbin/sysctl", "-w", a) for a in ASSIGNMENTS)
    expected.append(("sudo", "-n", "-l", "-l"))
    expected.extend(("sudo", "-n", "-u", "root", "--", "/usr/sbin/sysctl", "-w", a) for a in ASSIGNMENTS)
    assert calls == expected, calls


def real_fakes_accept_and_restore() -> None:
    # Given
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        boundary, state = fake_setting(root)
        # When
        assert boundary.read() == 65536
        boundary.preflight()
        boundary.write(1048576)
        assert boundary.read() == 1048576
        boundary.write(65536)
        # Then
        assert state.read_text() == "65536"
        calls = [json.loads(line) for line in (root / "argv.jsonl").read_text().splitlines()]
        assert all(call[1:] == ["-n", "fs.inotify.max_user_watches"] for call in calls if call[0].endswith("fake-sysctl"))


def preflight_denials_never_mutate() -> None:
    for environment, operation in (({"AA_FAKE_WRITE_OK": "0"}, "preflight_write_unauthorized"),
                                   ({"AA_FAKE_LIST_FAIL": "1"}, "nonzero_exit"),
                                   ({"AA_FAKE_PASSWD": "1"}, "preflight_passwordless_unproven")):
        # Given
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            boundary, state = fake_setting(Path(directory))
            with patch.dict("os.environ", environment):
                # When / Then
                try: boundary.preflight()
                except SettingError as error: assert error.operation == operation, error
                else: raise AssertionError("denied authority accepted")
            assert state.read_text() == "65536"


def read_faults_are_typed_and_unprivileged() -> None:
    for mode, operation in (("malformed", "parse"), ("nonzero", "nonzero_exit"), ("hang", "timeout"), ("missing", "launch_io")):
        # Given
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            boundary, state = fake_setting(root)
            if mode == "missing":
                boundary = SudoSetting(sudo=boundary.sudo, sysctl=str(root / "missing"))
            with patch.dict("os.environ", {"AA_FAKE_READ": mode}):
                started = time.monotonic()
                # When / Then
                try: boundary.read(timeout=0.3)
                except SettingError as error:
                    assert error.operation == operation, error
                    assert boundary.sysctl in str(error), error
                else: raise AssertionError("faulty read accepted")
                assert time.monotonic() - started < 0.6
            assert state.read_text() == "65536"


def preflight_shares_absolute_five_second_budget() -> None:
    # Given: a deterministic clock charging each subprocess.
    clock = [100.0]
    deadlines: list[float] = []
    def execute(argv: tuple[str, ...], deadline: float) -> subprocess.CompletedProcess[str]:
        deadlines.append(deadline)
        clock[0] += 1.0
        return subprocess.CompletedProcess(argv, 0, POLICY, "")
    with patch.object(setting.time, "monotonic", side_effect=lambda: clock[0]), patch.object(setting, "execute", execute):
        # When
        SudoSetting(timeout_seconds=20).preflight(timeout=30)
    # Then
    assert deadlines == [105.0, 105.0, 105.0], deadlines


def retry_route_is_exclusive_and_qualification_gated() -> None:
    # Given: only synthetic destination and mocked transport.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        destination = root / "task-5-prerequisites-ceiling-retry.json"
        argv = ["stage1.py", "--receipt", str(destination)]
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", argv), \
                patch.object(stage1, "attempt_target", return_value=17) as target:
            # When
            assert stage1.main() == 17
            # Then
            assert target.call_args.args[1] == destination
        failed = stage1.Receipt(argv=[], command="fake", cwd=directory, exit_status=1, stdout="", stderr="expected qualification failure")
        with patch.object(stage1, "run", return_value=failed), patch.object(stage1, "run_owned") as transport:
            with ExitStack() as stack: assert stage1.attempt_target(stack, destination) == 1
            transport.assert_not_called()
            assert not destination.exists()
        destination.write_bytes(b"historical retry")
        passed = stage1.Receipt(argv=[], command="fake", cwd=directory, exit_status=0, stdout="", stderr="")
        with patch.object(stage1, "run", return_value=passed), patch.object(stage1, "run_owned") as transport:
            try:
                with ExitStack() as stack: stage1.attempt_target(stack, destination)
            except FileExistsError: transport.assert_not_called()
            else: raise AssertionError("retry receipt overwritten")
            transport.assert_not_called()
        assert destination.read_bytes() == b"historical retry"


def rss_route_cli_exact_name_accepted_and_suffix_refused() -> None:
    # Given: only synthetic destinations and mocked executors.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        destination = root / "task-5-topology-sizing-rss-lifecycle.json"
        argv = ["stage1.py", "--receipt", str(destination)]
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", argv), \
                patch.object(stage1, "attempt_sizing", return_value=19) as sizing:
            # When
            assert stage1.main() == 19
            # Then: only the exact name dispatches to the sizing executor.
            assert sizing.call_args.args[1] == destination
        # When: any arbitrary suffix spelling. Then: refused before qualification and transport.
        for name in (str(destination) + ".bak", str(destination) + "x",
                     str(destination).replace(".json", "-2.json"), str(destination).replace(".json", ".jsonx")):
            with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", name]), \
                    patch.object(stage1, "run") as qualify, patch.object(stage1, "run_owned") as transport:
                try: stage1.main()
                except stage1.ReceiptDestinationError as error: assert error.arguments == ["--receipt", name]
                else: raise AssertionError("arbitrary suffix accepted")
                qualify.assert_not_called()
                transport.assert_not_called()
        assert not list(root.iterdir())


def errors_report_actual_argv() -> None:
    # Given: subprocess failures captured without invoking real commands.
    actions: tuple[Literal["read", "write", "preflight"], ...] = ("read", "write", "preflight")
    for action in actions:
        calls: list[tuple[str, ...]] = []
        def execute(argv: tuple[str, ...], deadline: float) -> subprocess.CompletedProcess[str]:
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 7, "", "denied")
        with patch.object(setting, "execute", execute):
            # When / Then
            try:
                boundary = SudoSetting()
                match action:
                    case "read": boundary.read()
                    case "write": boundary.write(65536)
                    case "preflight": boundary.preflight()
                    case unreachable: assert_never(unreachable)
            except SettingError as error: assert repr(calls[-1]) in str(error), error
            else: raise AssertionError("nonzero accepted")


def noncanonical_executables_block_before_probes() -> None:
    # Given: relative and symlink executable paths.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        boundary, _ = fake_setting(root)
        link = root / "linked-sysctl"
        link.symlink_to(boundary.sysctl)
        for executable in ("fake-sysctl", str(link)):
            with patch.object(setting, "execute") as execute:
                # When / Then
                try: SudoSetting(sysctl=executable).preflight()
                except SettingError as error: assert error.operation == "preflight_executable"
                else: raise AssertionError("noncanonical executable accepted")
                execute.assert_not_called()


def main() -> int:
    failures = 0
    for test in (exact_production_argv, real_fakes_accept_and_restore, preflight_denials_never_mutate,
                 read_faults_are_typed_and_unprivileged, preflight_shares_absolute_five_second_budget,
                 retry_route_is_exclusive_and_qualification_gated, rss_route_cli_exact_name_accepted_and_suffix_refused,
                 errors_report_actual_argv,
                 noncanonical_executables_block_before_probes):
        try: test()
        except (AssertionError, SettingError, stage1.ReceiptDestinationError) as error:
            failures += 1
            print(f"FAIL {test.__name__}: {error}")
        else: print(f"PASS {test.__name__}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
