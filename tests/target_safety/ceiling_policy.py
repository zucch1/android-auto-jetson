# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_policy.py
"""Observed-policy qualification using only local fixtures and fake executables."""
from __future__ import annotations

import io
import json
import sys
import tempfile
from contextlib import ExitStack, redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import stage1
from target_safety.guard import GuardChild
from target_safety.runner import RunResult
from target_safety.policy import require_passwordless
from target_safety.setting import SettingError
from target_safety.supervisor import Supervisor
from ceiling_budget import COMMANDS
from ceiling_setting_fixtures import fake_setting

OBSERVED = json.loads((stage1.EVIDENCE / "task-5-ssh-key-and-policy-inspection.json").read_text())["read_only_policy_query"]["observed_stdout"]


def observed_inspection_is_accepted() -> None:
    # Given: exact immutable inspection stdout. When / Then: both writes qualified.
    require_passwordless(OBSERVED, COMMANDS)


def source_labels_outside_exact_ordered_profile_are_rejected() -> None:
    # Given: renamed, swapped, or duplicated labels with unchanged rule bodies.
    first, second = "/etc/sudoers", "/etc/sudoers.d/90-cloud-init-users"
    variants = (
        OBSERVED.replace(first + "\n", "/etc/sudoers.d/z-last\n"),
        OBSERVED.replace(second, "/etc/sudoers.d/a-first"),
        OBSERVED.replace(first + "\n", second + "\n").replace(second + "\n    RunAsUsers: ALL\n    Options", first + "\n    RunAsUsers: ALL\n    Options"),
        OBSERVED.replace(second, first),
        OBSERVED.replace(first + "\n", second + "\n"),
    )
    accepted = []
    for index, raw in enumerate(variants):
        # When: qualify one mutated profile. Then: reject, regardless of labels' authority.
        try:
            require_passwordless(raw, COMMANDS)
        except SettingError as error:
            assert error.operation == "preflight_passwordless_unproven"
        else:
            accepted.append(index)
    assert not accepted, f"unsupported source label variants accepted: {accepted}"
    print(f"PASS {len(variants)} source label mutations rejected")


def unsupported_observed_topologies_are_rejected() -> None:
    # Given: every mutation is outside the single fully consumed observed shape.
    header, first, last = OBSERVED.split("Sudoers entry: ")
    variants = [
        header + "Sudoers entry: " + last + "Sudoers entry: " + first,
        OBSERVED + "Sudoers entry: " + first,
        OBSERVED + "Sudoers entry: " + last.replace("!authenticate", "authenticate"),
        OBSERVED + "Sudoers entry: " + last.replace("\tALL", "\t" + COMMANDS[0]),
        OBSERVED.replace("    Options: !authenticate\n", ""),
        OBSERVED.replace("!authenticate", "authenticate"),
        header + "Sudoers entry: " + first.replace("RunAsGroups: ALL", "Options: !authenticate")
        + "Sudoers entry: " + last.replace("!authenticate", "authenticate"),
        OBSERVED.replace("RunAsGroups: ALL", "Options: !authenticate"),
        OBSERVED.replace("Commands:\n\tALL", "Commands:\n\t!ALL"),
        OBSERVED.replace("RunAsUsers: ALL", "RunAsUsers: ROOTS"),
        OBSERVED.replace("RunAsGroups: ALL", "RunAsGroups: root"),
        OBSERVED.replace("!authenticate", "!authenticate, noexec"),
        OBSERVED.replace("!authenticate", "!authenticate\n    RunAsGroups: ALL"),
        OBSERVED.replace("!authenticate", "!authenticate\n    Options: !authenticate"),
        OBSERVED.replace("/etc/sudoers.d/90-cloud-init-users", "ROOT_POLICY"),
        OBSERVED.replace("/etc/sudoers.d/90-cloud-init-users", "/etc/sudoers.d/*"),
        OBSERVED.replace("/etc/sudoers.d/90-cloud-init-users", "/etc/../sudoers"),
        OBSERVED.replace("/snap/bin", "/other/bin"),
        OBSERVED.replace(r"\:", ":"),
        header + OBSERVED,
        OBSERVED + "unknown trailing text\n",
        OBSERVED.replace("User zucchi", "User other"),
        OBSERVED.replace("on jetson:\n\nSudoers", "on other:\n\nSudoers"),
        header + "Sudoers entry: " + first + "Sudoers entry: " + last.replace("\tALL", "\t/usr/bin/id"),
        header + "Sudoers entry: " + first + "Sudoers entry: " + last.replace("RunAsUsers: ALL", "RunAsUsers: !root"),
        OBSERVED.replace(header.splitlines()[0] + "\n", ""),
        OBSERVED.replace(header.splitlines()[1] + "\n", ""),
        OBSERVED.replace("RunAsGroups: ALL", "RunAsGroups: ALL\n    Options: authenticate"),
        header + "Sudoers entry: " + first + "Sudoers entry: " + last.replace("RunAsUsers: ALL", "RunAsUsers: root"),
    ]
    for token in ("authenticate", "!authenticate", "rootpw", "targetpw", "runaspw",
                  "exempt_group=wheel", "unknown_default", "env_reset", "mail_badpass", "use_pty"):
        variants.append(OBSERVED.replace("env_reset,", f"{token}, env_reset,"))
    variants.append(OBSERVED.replace("use_pty", "use_pty, " + header.splitlines()[1].strip().split(", ")[2]))
    for command in ("*", "ROOT_COMMANDS", "ALL, !/usr/bin/id", "NOPASSWD: ALL",
                    "sha256:abcdef ALL", "/usr/bin/id", COMMANDS[0]):
        variants.append(OBSERVED.replace("\tALL", "\t" + command))
    for token in ("env_reset, ", "mail_badpass, ", ", use_pty"):
        variants.append(OBSERVED.replace(token, ""))
    for raw in variants:
        # When / Then: rejection, never presence-based NOPASSWD acceptance.
        try:
            require_passwordless(raw, COMMANDS)
        except SettingError as error:
            assert error.operation == "preflight_passwordless_unproven"
        else:
            raise AssertionError(f"unsupported observed topology accepted: {raw!r}")
    print(f"PASS {len(variants)} unsupported observed topology mutations")


def observed_fakes_probe_then_raise_and_restore() -> None:
    # Given: separate fake-sudo and unprivileged fake-read executable.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        boundary, state = fake_setting(root)
        (root / "policy.txt").write_text(OBSERVED)
        # When: fresh probes precede full-list interpretation and exact writes.
        boundary.preflight()
        boundary.write(1048576)
        boundary.write(65536)
        # Then: no broader command is executed despite ALL in the listing.
        calls = [json.loads(line)[1:] for line in (root / "argv.jsonl").read_text().splitlines()]
        assignments = [f"fs.inotify.max_user_watches={value}" for value in (65536, 1048576)]
        expected = [["-n", "-u", "root", "-l", "--", boundary.sysctl, "-w", a] for a in assignments]
        expected.append(["-n", "-l", "-l"])
        expected.extend(["-n", "-u", "root", "--", boundary.sysctl, "-w", a] for a in reversed(assignments))
        assert calls == expected and state.read_text() == "65536", calls
        for value in (0, 1, 1048575, 1048577):
            try: boundary.write(value)
            except SettingError as error: assert error.operation == "refused_write"
            else: raise AssertionError("ALL expanded allowed writes")


def denied_observed_authority_never_raises_or_spawns_guard() -> None:
    for environment, policy in (({"AA_FAKE_WRITE_OK": "0"}, OBSERVED),
                                ({"AA_FAKE_DENY_TEMPORARY": "1"}, OBSERVED),
                                ({"AA_FAKE_LIST_FAIL": "1"}, OBSERVED),
                                ({}, OBSERVED.replace("!authenticate", "authenticate"))):
        # Given: denied exact probe, failed list, or rejected final authentication.
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            boundary, state = fake_setting(root)
            (root / "policy.txt").write_text(policy)
            with patch.dict("os.environ", environment), patch.object(GuardChild, "spawn") as spawn, redirect_stdout(io.StringIO()):
                # When: real supervisor gate with local fake executables only.
                outcome = Supervisor(boundary, (sys.executable, "-c", "pass")).run()
            # Then: no raise, no guard, unchanged synthetic original.
            assert not outcome.accepted and state.read_text() == "65536"
            spawn.assert_not_called()
            calls = [json.loads(line)[1:] for line in (root / "argv.jsonl").read_text().splitlines()]
            assert not any("--" in call and "-l" not in call for call in calls), calls


def policy_route_is_exclusive_and_qualification_gated() -> None:
    # Given: temporary evidence root and mocked transport (zero actual SSH).
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        destination = root / "task-5-prerequisites-ceiling-policy.json"
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), patch.object(stage1, "attempt_target", return_value=19) as target:
            # When / Then: allowlisted route uses the existing attempt, not a new executor.
            assert stage1.main() == 19
            assert target.call_args.args[1] == destination
        for arguments in (["--receipt", str(root / "other-policy.json")],
                          ["--receipt", str(destination), "--retry"]):
            with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", *arguments]), patch.object(stage1, "run_owned") as transport:
                try: stage1.main()
                except stage1.ReceiptDestinationError: transport.assert_not_called()
                else: raise AssertionError("non-allowlisted policy destination accepted")
        failed = stage1.Receipt(argv=[], command="fake", cwd=directory, exit_status=1, stdout="", stderr="expected policy qualification failure")
        with patch.object(stage1, "run", return_value=failed), patch.object(stage1, "run_owned") as transport:
            with ExitStack() as stack: assert stage1.attempt_target(stack, destination) == 1
            transport.assert_not_called()
            assert not destination.exists()
        destination.write_bytes(b"historical policy attempt")
        passed = stage1.Receipt(argv=[], command="fake", cwd=directory, exit_status=0, stdout="", stderr="")
        with patch.object(stage1, "run", return_value=passed), patch.object(stage1, "run_owned") as transport:
            try:
                with ExitStack() as stack: stage1.attempt_target(stack, destination)
            except FileExistsError: transport.assert_not_called()
            else: raise AssertionError("policy receipt overwritten")
        assert destination.read_bytes() == b"historical policy attempt"


def resumed_route_reaches_existing_executor() -> None:
    # Given: a fresh fixed route in a private evidence root; no real transport.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        destination = root / "task-5-prerequisites-ceiling-policy-resumed.json"
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), patch.object(stage1, "attempt_target", return_value=19) as target:
            # When / Then: main dispatches to the existing executor exactly once.
            assert stage1.main() == 19
            target.assert_called_once()
            assert target.call_args.args[1] == destination
        passed = stage1.Receipt(argv=[], command="fake", cwd=directory, exit_status=0, stdout="", stderr="")
        remote = RunResult(argv=stage1.SSH, exit_status=0, stdout="misleading success", stderr="",
                            timed_out=False, evidence_complete=True)
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), patch.object(stage1, "run", return_value=passed), patch.object(stage1, "run_owned", return_value=remote) as transport:
            # When: the real executor runs with only its transport boundary mocked.
            assert stage1.main() == 1
            # Then: one reserved receipt, unchanged payload, no false success claim.
            transport.assert_called_once()
            captured = transport.call_args.kwargs['capture']
            transport.assert_called_once_with(stage1.SSH, capture=captured, timeout_seconds=stage1.LOCAL_RUN_TIMEOUT_SECONDS, stdin=stage1.supervisor_bundle(), cwd=stage1.WORKTREE)
            assert all(Path(path).is_file() for path in captured.paths.values())
        receipt = json.loads(destination.read_text())
        assert not receipt["accepted"] and not receipt["task5_completed"]


def resumed_route_rejects_malformed_and_extra_arguments() -> None:
    # Given: malformed spellings, arbitrary names, missing values and extra argv.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        destination = root / "task-5-prerequisites-ceiling-policy-resumed.json"
        for arguments in (["--receipt"], [str(destination)], ["--receipt=" + str(destination)],
                          ["--receipt", str(root / "other-resumed.json")],
                          ["--receipt", str(destination), "--retry"]):
            with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", *arguments]), patch.object(stage1, "run") as qualify, patch.object(stage1, "run_owned") as transport:
                # When / Then: invalid argv is rejected before qualification or transport.
                try: stage1.main()
                except stage1.ReceiptDestinationError:
                    qualify.assert_not_called()
                    transport.assert_not_called()
                else: raise AssertionError("invalid resume argv accepted")


def resumed_route_is_qualification_gated_and_exclusive() -> None:
    # Given: old policy history plus a fresh, existing, or dangling resume route.
    for state in ("fresh", "regular", "dangling"):
        with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
            root = Path(directory)
            history = root / "task-5-prerequisites-ceiling-policy.json"
            history.write_bytes(b"historical policy attempt")
            destination = root / "task-5-prerequisites-ceiling-policy-resumed.json"
            if state == "regular": destination.write_bytes(b"historical resume attempt")
            if state == "dangling": destination.symlink_to(root / "missing.json")
            qualification = stage1.Receipt(argv=[], command="fake", cwd=directory, exit_status=int(state == "fresh"), stdout="", stderr="expected resume qualification failure" if state == "fresh" else "")
            with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), patch.object(stage1, "run", return_value=qualification), patch.object(stage1, "run_owned") as transport:
                # When: main reaches the existing qualification/exclusive-open gate.
                if state == "fresh": assert stage1.main() == 1
                else:
                    try: stage1.main()
                    except FileExistsError: transport.assert_not_called()
                    else: raise AssertionError("occupied resume receipt accepted")
                # Then: zero transports and history/link/absence are preserved.
                transport.assert_not_called()
            assert history.read_bytes() == b"historical policy attempt"
            if state == "fresh": assert not (destination.exists() or destination.is_symlink())
            if state == "regular": assert destination.read_bytes() == b"historical resume attempt"
            if state == "dangling": assert destination.is_symlink() and not (root / "missing.json").exists()


def main() -> int:
    failures = 0
    for test in (observed_inspection_is_accepted, source_labels_outside_exact_ordered_profile_are_rejected,
                 unsupported_observed_topologies_are_rejected, observed_fakes_probe_then_raise_and_restore,
                 denied_observed_authority_never_raises_or_spawns_guard, policy_route_is_exclusive_and_qualification_gated,
                 resumed_route_reaches_existing_executor, resumed_route_rejects_malformed_and_extra_arguments,
                 resumed_route_is_qualification_gated_and_exclusive):
        try: test()
        except (AssertionError, SettingError, stage1.ReceiptDestinationError) as error:
            failures += 1
            print(f"FAIL {test.__name__}: {error}")
        else: print(f"PASS {test.__name__}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
