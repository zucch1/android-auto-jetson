"""Unprivileged bounded adapter for the owner-approved isolated proc helper."""
from __future__ import annotations

import json
import tempfile
from pathlib import Path

from .acquire_models import ProcessObservation
from .acquire_proc_helper import OUTPUT_LIMIT, PROCESS_LIMIT, TIMEOUT_SECONDS
from .acquire_transport_io import run_bounded
from .kernel import Blocked


def helper_argv() -> tuple[str, ...]:
    return ("/usr/bin/sudo", "-n", "--", "/usr/bin/python3", "-I", "-B",
            str(Path(__file__).resolve().with_name("acquire_proc_helper.py")))


def parse_sample(raw: bytes) -> ProcessObservation:
    data = json.loads(raw)
    if (not isinstance(data, dict) or data.get("schema") != "aa-acquire-proc/1"
            or type(data.get("complete")) is not bool or type(data.get("euid")) is not int
            or data["euid"] != 0):
        raise Blocked("proc_helper_malformed", "schema/access")
    entries = data.get("processes")
    if not isinstance(entries, list) or len(entries) > PROCESS_LIMIT:
        raise Blocked("proc_helper_malformed", "processes")
    processes: list[tuple[str, str, str | None]] = []
    seen: set[str] = set()
    for entry in entries:
        if (not isinstance(entry, list) or len(entry) != 3 or not isinstance(entry[0], str)
                or not entry[0].isdigit() or entry[0] in seen or not isinstance(entry[1], str)
                or not (entry[2] is None or isinstance(entry[2], str))):
            raise Blocked("proc_helper_malformed", "entry")
        seen.add(entry[0])
        processes.append((entry[0], entry[1], entry[2]))
    return ProcessObservation(tuple(processes), None, data["complete"])


def observe_processes() -> ProcessObservation:
    try:
        with tempfile.TemporaryDirectory(prefix="aa-proc-", dir="/tmp") as directory:
            output = Path(directory) / "sample.json"
            result = run_bounded(helper_argv(), b"", output, OUTPUT_LIMIT, TIMEOUT_SECONDS + 2)
            if result.exit_status != 0 or result.timed_out:
                return ProcessObservation((), None, False)
            return parse_sample(output.read_bytes())
    except (Blocked, OSError, ValueError, TypeError):
        return ProcessObservation((), None, False)
