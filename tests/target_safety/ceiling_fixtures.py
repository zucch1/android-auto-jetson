# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Local guard-child fixtures for the ceiling fault suite; never a kernel mutation."""
from __future__ import annotations

import sys
from pathlib import Path
from typing import Final

MARKER: Final = "protected_no_detected_write_no_persistent_change"

_GUARD_SOURCE: Final = """\
import json, os, signal, subprocess, sys, time
def emit(event):
    print(json.dumps({"event": event, "data": "ok", "monotonic_ns": time.monotonic_ns()}), flush=True)
mode = sys.argv[1]
if mode == "success":
    emit("__MARKER__")
elif mode == "fail":
    emit("__MARKER__")
    sys.exit(3)
elif mode == "spoof":
    print("prose mentioning __MARKER__ but not json", flush=True)
elif mode == "malformed":
    print("{\\"event\\": __MARKER__", flush=True)
elif mode == "crash":
    os.kill(os.getpid(), signal.SIGKILL)
elif mode == "hang":
    emit("__MARKER__")
    time.sleep(60)
elif mode == "ignore-term":
    g = subprocess.Popen([sys.executable, "-c",
        "import signal, time\\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\\ntime.sleep(60)"])
    open(sys.argv[2], "w").write(str(g.pid))
    time.sleep(60)
elif mode == "leak-hold":
    g = subprocess.Popen([sys.executable, "-c",
        "import os, signal, time\\nos.setsid()\\nsignal.signal(signal.SIGTERM, signal.SIG_IGN)\\ntime.sleep(60)"])
    open(sys.argv[2], "w").write(str(g.pid))
    time.sleep(60)
elif mode == "partial-hang":
    print("partial-output", flush=True)
    time.sleep(60)
else:
    sys.exit(2)
"""


def guard_argv(tmpdir: Path, mode: str, *extra: str) -> tuple[str, ...]:
    """Return argv for one fixture guard child in the named lifecycle mode."""
    script = tmpdir / "fixture-guard.py"
    if not script.exists():
        script.write_text(_GUARD_SOURCE.replace("__MARKER__", MARKER))
    return (sys.executable, "-B", str(script), mode, *extra)
