# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_sqlite_route.py
"""Fresh fixed topology-sizing sqlite-absence route boundary tests; local transport only.

Focused route-safety coverage for the one fixed receipt name
`task-5-topology-sizing-sqlite-absence.json` (routes.SIZING_SQLITE_ABSENCE, kind
"sizing"). Transport is a tiny local substitute seam (patched stage1.SSH), never
real SSH and never a target. Independent of the pending Oracle coverage design;
not wired into ceiling_qa here (the parent core worker integrates later).
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Final
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import routes, stage1

NAME: Final = "task-5-topology-sizing-sqlite-absence.json"
SUFFIXES: Final = (".remote.stdout", ".remote.stderr", ".lifecycle.jsonl")
SIZING_ADMISSION: Final = 3221225472
CEILING_ADMISSION: Final = 4294967296


def qualification(code: int) -> stage1.Receipt:
    return stage1.Receipt(argv=[], command="local fixture", cwd="/tmp/opencode",
                          exit_status=code, stdout="", stderr="qualification fixture")


def exact_dispatch_reaches_sizing_executor_once() -> None:
    root = Path(tempfile.mkdtemp(prefix="sqlite-route-dispatch-", dir="/tmp/opencode"))
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
    assert routes.SIZING_SQLITE_ABSENCE == NAME
    assert [kind for name, kind in routes._table() if name == NAME] == ["sizing"]
    print("PASS sqlite-absence route exact dispatch reaches the sizing executor exactly once")


def arbitrary_suffixes_and_extra_args_reject_before_qualification() -> None:
    root = Path(tempfile.mkdtemp(prefix="sqlite-route-argv-", dir="/tmp/opencode"))
    destination = root / NAME
    spellings: list[list[str]] = [
        ["--receipt"],
        ["--receipt=" + str(destination)],
        [str(destination)],
        ["--receipt", str(root / "other.json")],
        ["--receipt", str(destination), "--retry"],
        ["--receipt", str(destination), "--mode", "sizing"],
        ["--receipt", str(destination), "--receipt", str(destination)],
    ]
    for name in (NAME + ".bak", NAME + "x", NAME.replace(".json", ".jsonx"),
                 NAME.replace(".json", "-2.json"), NAME.replace(".json", ".remote.stdout"),
                 NAME.replace("sqlite-absence", "sqlite-absence2"), NAME.upper()):
        spellings.append(["--receipt", str(root / name)])
    for arguments in spellings:
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", *arguments]), \
                patch.object(stage1, "run") as qualify, patch.object(stage1, "run_owned") as transport:
            try:
                stage1.main()
            except routes.ReceiptDestinationError as error:
                assert error.arguments == arguments
            else:
                raise AssertionError(f"malformed sqlite-absence spelling accepted: {arguments}")
            qualify.assert_not_called()
            transport.assert_not_called()
            assert not list(root.iterdir())
    print("PASS sqlite-absence route rejects arbitrary suffixes and extra args before qualification/transport")


def qualification_failure_reserves_nothing() -> None:
    root = Path(tempfile.mkdtemp(prefix="sqlite-route-qual-", dir="/tmp/opencode"))
    destination = root / NAME
    with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
            patch.object(stage1, "run", return_value=qualification(7)) as qualify, \
            patch.object(stage1, "run_owned") as transport:
        assert stage1.main() == 1
        qualify.assert_called_once()
        transport.assert_not_called()
        assert not list(root.iterdir())
    print("PASS sqlite-absence route qualification failure reserves nothing and transports nothing")


def collisions_preserve_history_without_overwrite_or_popen() -> None:
    for suffix in (".json", *SUFFIXES):
        for dangling in (False, True):
            root = Path(tempfile.mkdtemp(prefix="sqlite-route-collision-", dir="/tmp/opencode"))
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
                    patch.object(stage1, "run_owned") as transport, \
                    patch.object(subprocess, "Popen") as popen:
                try:
                    stage1.main()
                except FileExistsError:
                    transport.assert_not_called()
                    popen.assert_not_called()
                else:
                    raise AssertionError("sqlite-absence collision allowed transport")
            assert history.read_bytes() == b"unchanged history"
            assert os.path.lexists(occupied)
            if dangling:
                assert occupied.is_symlink() and os.readlink(occupied) == str(root / "absent")
            else:
                assert occupied.read_bytes() == b"occupied history"
    print("PASS sqlite-absence collisions (regular/dangling) preserve history, no overwrite, no Popen")


def policy_echo_sizing_3gib_vs_ceiling_4gib() -> None:
    # Tiny LOCAL substitute transport seam (patched stage1.SSH), never real SSH.
    child = (sys.executable, "-B", "-c",
             "import os,sys; sys.stdin.buffer.read(); os.write(1,b'misleading\\xff'); os.write(2,b'err')")
    for name, admission, sizing in ((NAME, SIZING_ADMISSION, True),
                                    ("task-5-prerequisites-ceiling-attempt.json", CEILING_ADMISSION, False)):
        root = Path(tempfile.mkdtemp(prefix="sqlite-route-policy-", dir="/tmp/opencode"))
        destination = root / name
        with patch.object(stage1, "EVIDENCE", root), patch.object(sys, "argv", ["stage1.py", "--receipt", str(destination)]), \
                patch.object(stage1, "run", return_value=qualification(0)), patch.object(stage1, "SSH", child):
            assert stage1.main() == 1
        policy = json.loads(destination.read_text())["resource_policy"]
        assert policy["admission_mem_available_bytes"] == admission, (name, policy)
        if sizing:
            assert policy == dict(stage1.SIZING_RESOURCE_POLICY), policy
            assert policy["admission_scope"] == "topology-sizing-only", policy
            assert policy["ceiling_admission_mem_available_bytes"] == CEILING_ADMISSION, policy
        else:
            assert policy == dict(stage1.RESOURCE_POLICY), policy
            assert "admission_scope" not in policy, policy
    print("PASS sqlite-absence receipt echoes SIZING MODE 3GiB policy vs ceiling 4GiB")


def main() -> int:
    exact_dispatch_reaches_sizing_executor_once()
    arbitrary_suffixes_and_extra_args_reject_before_qualification()
    qualification_failure_reserves_nothing()
    collisions_preserve_history_without_overwrite_or_popen()
    policy_echo_sizing_3gib_vs_ceiling_4gib()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
