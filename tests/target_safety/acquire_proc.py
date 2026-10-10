# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_proc.py
"""B5 regressions for full bounded proc visibility and privilege isolation."""
from __future__ import annotations

import sys
import json
import os
import subprocess
import tempfile
import signal
import io
from types import SimpleNamespace
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import target_safety.acquire_readers as readers
import target_safety.acquire_proc as adapter
import target_safety.acquire_proc_helper as helper
from target_safety.acquire_transport_io import TransportReceipt


def test_sample_when_more_than_400_processes() -> None:
    # Given 450 accessible processes, When sampled, Then the entire listing is complete.
    with patch.object(helper.os, "listdir", return_value=[str(i) for i in range(1, 451)]), \
            patch.object(helper, "_comm", return_value="init"), \
            patch.object(helper.os, "readlink", return_value="/"):
        sample = helper.scan()
    assert sample.complete and len(sample.processes) == 450, (sample.complete, len(sample.processes))


def test_sample_when_limit_or_access_unknown() -> None:
    # Given a limit excess or denied cwd, When scanned, Then the sample is incomplete.
    with patch.object(helper.os, "listdir", return_value=[str(i) for i in range(1, helper.PROCESS_LIMIT + 2)]):
        sample = helper.scan()
    assert not sample.complete and sample.reason == "process_limit"
    with patch.object(helper.os, "listdir", return_value=["1"]), \
            patch.object(helper, "_comm", return_value="root-worker"), \
            patch.object(helper.os, "readlink", side_effect=PermissionError("cwd denied")):
        sample = helper.scan()
    assert not sample.complete and sample.processes == (("1", "root-worker", None),)


def test_sample_when_busy_is_not_clean() -> None:
    # Given a real relevant-cwd value in helper JSON, When parsed, Then the quiet gate refuses it.
    raw = json.dumps({"schema": "aa-acquire-proc/1", "euid": 0, "complete": True,
                      "processes": [["9", "editor", "/protected/repo"]]}).encode()
    sample = adapter.parse_sample(raw)
    assert not readers.assess_quiet(sample, sample, (Path("/protected"),)).proven


def test_adapter_when_helper_fails() -> None:
    # Given denied sudo or timeout, When observed, Then it never fabricates a clean sample.
    for receipt in (TransportReceipt(1, "", 0, "sudo denied", False),
                    TransportReceipt(124, "", 0, "timeout", True)):
        with patch.object(adapter, "run_bounded", return_value=receipt):
            sample = adapter.observe_processes()
        assert not sample.complete and not sample.processes


def test_helper_when_arguments_or_user_modules_supplied() -> None:
    # Given poisoned PYTHONPATH/cwd and an extra argument, When isolated helper runs, Then injection is ignored/args rejected.
    with tempfile.TemporaryDirectory(prefix="task5-proc-isolation-", dir="/tmp/opencode") as directory:
        base = Path(directory)
        (base / "json.py").write_text("raise RuntimeError('untrusted json loaded')\n")
        (base / "sitecustomize.py").write_text("raise RuntimeError('user site loaded')\n")
        command = ["/usr/bin/python3", "-I", "-B", str(Path(helper.__file__).resolve())]
        result = subprocess.run([*command, "/arbitrary/input"], cwd=base, timeout=5,
                                capture_output=True, env={**os.environ, "PYTHONPATH": str(base)})
        assert result.returncode == 64 and not result.stdout and not result.stderr
        result = subprocess.run(command, cwd=base, timeout=12, capture_output=True,
                                env={**os.environ, "PYTHONPATH": str(base)})
        assert not result.stderr
        if os.geteuid() != 0:
            assert result.returncode == 70
            assert json.loads(result.stdout)["reason"] == "root_visibility_required"
    argv = adapter.helper_argv()
    assert argv[:6] == ("/usr/bin/sudo", "-n", "--", "/usr/bin/python3", "-I", "-B")
    assert Path(argv[6]).is_absolute() and Path(argv[6]).name == "acquire_proc_helper.py"


def test_adapter_when_malformed_or_unprivileged_output() -> None:
    # Given bad helper evidence, When parsed, Then the boundary rejects it.
    for value in ({"schema": "aa-acquire-proc/1", "euid": 1000, "complete": True, "processes": []},
                  {"schema": "aa-acquire-proc/1", "euid": 0, "complete": "yes", "processes": []},
                  {"schema": "aa-acquire-proc/1", "euid": 0, "complete": True,
                   "processes": [["9", "init", "/"], ["9", "init", "/"]]}):
        try:
            adapter.parse_sample(json.dumps(value).encode())
        except adapter.Blocked:
            pass
        else:
            raise AssertionError("invalid privileged observation accepted")


def test_helper_when_output_limit_reached() -> None:
    # Given oversized helper evidence, When emitted, Then the bounded failure replaces it.
    output = io.BytesIO()
    original_handler = signal.getsignal(signal.SIGALRM)
    try:
        with patch.object(helper.os, "geteuid", return_value=0), \
                patch.object(helper.sys, "argv", [helper.__file__]), \
                patch.object(helper.sys, "stdout", SimpleNamespace(buffer=output)), \
                patch.object(helper, "OUTPUT_LIMIT", 200), \
                patch.object(helper, "scan", return_value=helper.ProcSample(
                    (("1", "init", "/" + "x" * 400),), True, 0)):
            result = helper.main()
        assert result == 70 and len(output.getvalue()) <= 200
        assert json.loads(output.getvalue())["reason"] == "output_limit"
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, original_handler)


def test_helper_when_child_deadline_expires() -> None:
    # Given a stalled scan, When its own alarm fires, Then the privileged child exits failed.
    original_handler = signal.getsignal(signal.SIGALRM)
    try:
        with patch.object(helper.os, "geteuid", return_value=0), \
                patch.object(helper.sys, "argv", [helper.__file__]), \
                patch.object(helper, "scan", side_effect=lambda: signal.raise_signal(signal.SIGALRM)):
            try:
                helper.main()
            except SystemExit as error:
                assert error.code == 70
            else:
                raise AssertionError("helper child deadline ignored")
    finally:
        signal.alarm(0)
        signal.signal(signal.SIGALRM, original_handler)


if __name__ == "__main__":
    for test in (test_sample_when_more_than_400_processes, test_sample_when_limit_or_access_unknown,
                 test_sample_when_busy_is_not_clean, test_adapter_when_helper_fails,
                 test_helper_when_arguments_or_user_modules_supplied,
                 test_adapter_when_malformed_or_unprivileged_output,
                 test_helper_when_output_limit_reached, test_helper_when_child_deadline_expires):
        test()
        print("PASS", test.__name__)
