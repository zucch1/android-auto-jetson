# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_route.py
"""Fresh fixed topology-sizing route boundary tests; transport is local only."""
from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path
from typing import Final
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import routes, stage1

NAME: Final = "task-5-topology-sizing.json"
FRESH_NAME: Final = "task-5-topology-sizing-3gib.json"
RSS_NAME: Final = "task-5-topology-sizing-rss-lifecycle.json"
SUFFIXES: Final = (".remote.stdout", ".remote.stderr", ".lifecycle.jsonl")


def qualification(code: int) -> stage1.Receipt:
    return stage1.Receipt(argv=[], command="local fixture", cwd="/tmp/opencode",
                          exit_status=code, stdout="", stderr="qualification fixture")


def exact_route_calls_sizing_executor_once() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-dispatch-", dir="/tmp/opencode"))
    destination = root / NAME
    with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
            patch.object(stage1, "attempt_sizing", return_value=19) as sizing, \
            patch.object(stage1, "attempt_target") as target, patch.object(stage1, "attempt") as expanded:
        assert stage1.main() == 19
        sizing.assert_called_once()
        assert sizing.call_args.args[1] == destination
        target.assert_not_called()
        expanded.assert_not_called()
        assert not list(root.iterdir())
    assert routes.resolve(root, ["--receipt", str(destination)]) == (destination, "sizing")
    print("PASS sizing route exact dispatch reaches the sizing executor exactly once")


def malformed_arguments_reject_before_qualification() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-argv-", dir="/tmp/opencode"))
    destination = root / NAME
    for arguments in (["--receipt"], ["--receipt=" + str(destination)], [str(destination)],
                      ["--receipt", str(root / "other.json")], ["--receipt", str(destination), "--retry"],
                      ["--receipt", str(destination), "--mode", "sizing"],
                      ["--receipt", str(destination), "--receipt", str(destination)]):
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", *arguments]), \
                patch.object(stage1, "run") as qualify, patch.object(stage1, "run_owned") as transport:
            try:
                stage1.main()
            except routes.ReceiptDestinationError as error:
                assert error.arguments == arguments
            else:
                raise AssertionError("malformed sizing route accepted")
            qualify.assert_not_called()
            transport.assert_not_called()
            assert not list(root.iterdir())
    print("PASS sizing route rejects every malformed or extra spelling before qualification")


def qualification_failure_reserves_nothing() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-qual-", dir="/tmp/opencode"))
    destination = root / NAME
    with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
            patch.object(stage1, "run", return_value=qualification(7)) as qualify, \
            patch.object(stage1, "run_owned") as transport:
        assert stage1.main() == 1
        qualify.assert_called_once()
        transport.assert_not_called()
        assert not list(root.iterdir())
    print("PASS sizing route qualification failure reserves nothing and transports nothing")


def collisions_preserve_history_without_transport() -> None:
    for suffix in (".json", *SUFFIXES):
        for dangling in (False, True):
            root = Path(tempfile.mkdtemp(prefix="sizing-route-collision-", dir="/tmp/opencode"))
            destination = root / NAME
            occupied = destination.with_suffix(suffix)
            history = root / "historical.json"
            history.write_bytes(b"unchanged history")
            if dangling:
                occupied.symlink_to(root / "absent")
            else:
                occupied.write_bytes(b"occupied history")
            with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
                    patch.object(stage1, "run", return_value=qualification(0)), \
                    patch.object(stage1, "run_owned") as transport:
                try:
                    stage1.main()
                except FileExistsError:
                    transport.assert_not_called()
                else:
                    raise AssertionError("collision allowed transport")
            assert history.read_bytes() == b"unchanged history"
            assert os.path.lexists(occupied)
            if dangling:
                assert occupied.is_symlink() and os.readlink(occupied) == str(root / "absent")
            else:
                assert occupied.read_bytes() == b"occupied history"
    print("PASS sizing route collisions (regular/dangling) preserve history and block transport")


def misleading_or_ceiling_output_is_not_sizing_accepted() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-exit0-", dir="/tmp/opencode"))
    destination = root / NAME
    child = (sys.executable, "-B", "-c",
             "import os,sys; sys.stdin.buffer.read(); os.write(1,b'misleading success\\xff\\x00'); os.write(2,b'local error\\xfe')")
    payload_hash = hashlib.sha256(stage1.sizing_bundle()).hexdigest()
    with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
            patch.object(stage1, "run", return_value=qualification(0)), patch.object(stage1, "SSH", child):
        result = stage1.main()
    receipt = json.loads(destination.read_text())
    assert result == 1 and not receipt["accepted"] and receipt["remote"]["exit_status"] == 0
    assert receipt["schema"] == "aa-task5-topology-sizing-attempt-1"
    assert receipt["remote_stdin_sha256"] == payload_hash
    assert receipt["resource_policy"]["child_only_rlimit_as_bytes"] == 2147483648
    assert not receipt["task5_completed"]
    assert destination.with_suffix(".remote.stdout").read_bytes() == b"misleading success\xff\x00"
    events = [json.loads(line) for line in destination.with_suffix(".lifecycle.jsonl").read_text().splitlines()]
    assert [event["event"] for event in events] == ["reserved", "started", "terminal"]
    assert events[0]["invocation_count"] == 1 and events[-1]["reaped"]
    print("PASS sizing route exit-zero without a sizing outcome is never accepted")


def fresh_route_exact_dispatch_calls_sizing_executor_once() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-3gib-dispatch-", dir="/tmp/opencode"))
    destination = root / FRESH_NAME
    with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
            patch.object(stage1, "attempt_sizing", return_value=19) as sizing, \
            patch.object(stage1, "attempt_target") as target, patch.object(stage1, "attempt") as expanded:
        assert stage1.main() == 19
        sizing.assert_called_once()
        assert sizing.call_args.args[1] == destination
        target.assert_not_called()
        expanded.assert_not_called()
        assert not list(root.iterdir())
    assert routes.resolve(root, ["--receipt", str(destination)]) == (destination, "sizing")
    print("PASS fresh 3gib route exact dispatch reaches the sizing executor exactly once")


def fresh_route_malformed_arguments_reject_before_qualification() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-3gib-argv-", dir="/tmp/opencode"))
    destination = root / FRESH_NAME
    for arguments in (["--receipt"], ["--receipt=" + str(destination)], [str(destination)],
                      ["--receipt", str(root / "other.json")], ["--receipt", str(destination), "--retry"],
                      ["--receipt", str(destination), "--mode", "sizing"],
                      ["--receipt", str(destination), "--receipt", str(destination)]):
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", *arguments]), \
                patch.object(stage1, "run") as qualify, patch.object(stage1, "run_owned") as transport:
            try:
                stage1.main()
            except routes.ReceiptDestinationError as error:
                assert error.arguments == arguments
            else:
                raise AssertionError("malformed fresh 3gib route accepted")
            qualify.assert_not_called()
            transport.assert_not_called()
            assert not list(root.iterdir())
    print("PASS fresh 3gib route rejects every malformed or extra spelling before qualification")


def fresh_route_qualification_failure_reserves_nothing() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-3gib-qual-", dir="/tmp/opencode"))
    destination = root / FRESH_NAME
    with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
            patch.object(stage1, "run", return_value=qualification(7)) as qualify, \
            patch.object(stage1, "run_owned") as transport:
        assert stage1.main() == 1
        qualify.assert_called_once()
        transport.assert_not_called()
        assert not list(root.iterdir())
    print("PASS fresh 3gib route qualification failure reserves nothing and transports nothing")


def fresh_route_collisions_preserve_history_without_transport() -> None:
    for suffix in (".json", *SUFFIXES):
        for dangling in (False, True):
            root = Path(tempfile.mkdtemp(prefix="sizing-route-3gib-collision-", dir="/tmp/opencode"))
            destination = root / FRESH_NAME
            occupied = destination.with_suffix(suffix)
            history = root / "historical.json"
            history.write_bytes(b"unchanged history")
            if dangling:
                occupied.symlink_to(root / "absent")
            else:
                occupied.write_bytes(b"occupied history")
            with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
                    patch.object(stage1, "run", return_value=qualification(0)), \
                    patch.object(stage1, "run_owned") as transport:
                try:
                    stage1.main()
                except FileExistsError:
                    transport.assert_not_called()
                else:
                    raise AssertionError("fresh 3gib collision allowed transport")
            assert history.read_bytes() == b"unchanged history"
            assert os.path.lexists(occupied)
            if dangling:
                assert occupied.is_symlink() and os.readlink(occupied) == str(root / "absent")
            else:
                assert occupied.read_bytes() == b"occupied history"
    print("PASS fresh 3gib route collisions (regular/dangling) preserve history and block transport")


def rss_route_exact_dispatch_calls_sizing_executor_once() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-rss-dispatch-", dir="/tmp/opencode"))
    destination = root / RSS_NAME
    with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
            patch.object(stage1, "attempt_sizing", return_value=19) as sizing, \
            patch.object(stage1, "attempt_target") as target, patch.object(stage1, "attempt") as expanded:
        assert stage1.main() == 19
        sizing.assert_called_once()
        assert sizing.call_args.args[1] == destination
        assert not target.called and not expanded.called
        assert not list(root.iterdir())
    assert routes.resolve(root, ["--receipt", str(destination)]) == (destination, "sizing")
    print("PASS rss-lifecycle route exact name dispatch reaches the sizing executor exactly once")


def rss_route_arbitrary_suffixes_reject_before_qualification() -> None:
    root = Path(tempfile.mkdtemp(prefix="sizing-route-rss-suffix-", dir="/tmp/opencode"))
    for name in (RSS_NAME + ".bak", RSS_NAME + "x", RSS_NAME.replace(".json", ".jsonx"),
                 RSS_NAME.replace(".json", "-2.json"), RSS_NAME.replace(".json", ".remote.stdout"), RSS_NAME.upper()):
        destination = root / name
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
                patch.object(stage1, "run") as qualify, patch.object(stage1, "run_owned") as transport:
            try:
                stage1.main()
            except routes.ReceiptDestinationError as error:
                assert error.arguments == ["--receipt", str(destination)]
            else:
                raise AssertionError(f"arbitrary suffix accepted: {name}")
            assert not qualify.called and not transport.called
            assert not list(root.iterdir())
    print("PASS rss-lifecycle route refuses every arbitrary suffix before qualification")


def attempt_receipts_echo_actual_mode_policy() -> None:
    child = (sys.executable, "-B", "-c",
             "import os,sys; sys.stdin.buffer.read(); os.write(1,b'misleading\\xff'); os.write(2,b'err')")
    for name, admission, scope in ((FRESH_NAME, 3221225472, "topology-sizing-only"),
                                   (RSS_NAME, 3221225472, "topology-sizing-only"),
                                   ("task-5-prerequisites-ceiling-attempt.json", 4294967296, None)):
        root = Path(tempfile.mkdtemp(prefix="sizing-route-3gib-policy-", dir="/tmp/opencode"))
        destination = root / name
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
                patch.object(stage1, "run", return_value=qualification(0)), patch.object(stage1, "SSH", child):
            assert stage1.main() == 1
        receipt = json.loads(destination.read_text())
        policy = receipt["resource_policy"]
        assert policy["admission_mem_available_bytes"] == admission, (name, policy)
        if scope is None:
            assert "admission_scope" not in policy, policy
            assert set(policy) == set(stage1.RESOURCE_POLICY), policy
        else:
            assert policy["admission_scope"] == scope, policy
            assert policy["ceiling_admission_mem_available_bytes"] == 4294967296, policy
    print("PASS attempt receipts echo the actual mode policy (sizing 3GiB vs ceiling 4GiB)")


def main() -> int:
    exact_route_calls_sizing_executor_once()
    malformed_arguments_reject_before_qualification()
    qualification_failure_reserves_nothing()
    collisions_preserve_history_without_transport()
    misleading_or_ceiling_output_is_not_sizing_accepted()
    fresh_route_exact_dispatch_calls_sizing_executor_once()
    fresh_route_malformed_arguments_reject_before_qualification()
    fresh_route_qualification_failure_reserves_nothing()
    fresh_route_collisions_preserve_history_without_transport()
    rss_route_exact_dispatch_calls_sizing_executor_once()
    rss_route_arbitrary_suffixes_reject_before_qualification()
    attempt_receipts_echo_actual_mode_policy()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
