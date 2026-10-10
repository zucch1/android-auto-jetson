# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_bundle.py
"""Actual supervisor_bundle stdin execution with a fake setting + real Python stdin guard.

The real bundle's dependency entrypoint (module loading) is kept unmodified and
loads first; only the controlled entry (fake SudoSetting + nonempty guard stdin)
is injected before main(). Test-driver-memory only: no production change to the
SSH payload or target authority. The stdin CLI binary (python3 -B -) is run from
this parent driver and produces artifacts. No real SSH/sysctl/kernel mutation.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import stage1
from target_safety.setting import CEILING_ORIGINAL, CEILING_TEMPORARY
from ceiling_setting_fixtures import fake_setting

MARKER = "protected_no_detected_write_no_persistent_change"
_MARKER_BYTES = MARKER.encode()

# Real nonempty Python stdin guard: runs as `python3 -B -` reading this script
# from stdin, emits the exact final event. This is the production stdin path.
_GUARD_SCRIPT: bytes = (
    b"import json, sys, time\n"
    b"data = sys.stdin.buffer.read()\n"
    b"print(json.dumps({'event': '" + _MARKER_BYTES +
    b"', 'data': 'ok', 'monotonic_ns': time.monotonic_ns()}), flush=True)\n"
)


def bundle_preamble_is_unmodified() -> None:
    """Given the real bundle, Then the dependency entrypoint prefix loads before injection."""
    real = stage1.supervisor_bundle().decode()
    marker = "import target_safety.supervisor as _S"
    assert marker in real, "supervisor entrypoint marker missing"
    preamble = real[: real.index(marker)]
    assert real.startswith(preamble)
    for module in ("kernel", "coverage", "inventory", "probes", "setting", "guard", "report", "supervisor"):
        assert f"'target_safety.{module}'" in preamble, f"dependency {module} not loaded by entrypoint"
    assert "exec(compile(" in preamble
    print(f"PASS real supervisor_bundle dependency entrypoint loads unmodified before injection ({len(preamble)} bytes)")


def actual_bundle_stdin_with_fake_setting_and_guard() -> None:
    """Given the real bundle + fake setting + real Python stdin guard, Then accept and restore."""
    tmp = Path(tempfile.mkdtemp(prefix="ceiling-bundle-", dir="/tmp/opencode"))
    setting, state = fake_setting(tmp)
    real = stage1.supervisor_bundle().decode()
    preamble = real[: real.index("import target_safety.supervisor as _S")]
    injection = (
        "import target_safety.supervisor as _S\n"
        "import target_safety.setting as _st\n"
        f"fake = _st.SudoSetting(sudo={setting.sudo!r}, sysctl={setting.sysctl!r}, timeout_seconds=5.0)\n"
        f"guard = {_GUARD_SCRIPT!r}\n"
        "raise SystemExit(_S.main(setting=fake, guard_stdin=guard))\n"
    )
    payload = preamble + injection
    (tmp / "payload.py").write_text(payload)
    env = dict(os.environ, AA_FAKE_SETTING_FILE=str(state), AA_FAKE_WRITE_OK="1")
    result = subprocess.run([sys.executable, "-B", "-"], input=payload.encode(),
                            capture_output=True, timeout=60, env=env)
    (tmp / "stdout.txt").write_bytes(result.stdout)
    (tmp / "stderr.txt").write_bytes(result.stderr)
    assert result.returncode == 0, (result.stderr or result.stdout).decode(errors="replace")
    assert state.read_text() == str(CEILING_ORIGINAL), "setting must be restored to original"
    outcome = None
    for line in result.stdout.decode().splitlines():
        obj = json.loads(line)
        if obj.get("event") == "ceiling_outcome":
            outcome = obj["data"]
    assert outcome is not None, "no ceiling_outcome emitted"
    assert outcome["accepted"] is True
    assert outcome["original_observed"] == CEILING_ORIGINAL
    assert outcome["temp_observed"] == CEILING_TEMPORARY
    assert outcome["restore_observed"] == CEILING_ORIGINAL
    assert any(e["event"] == MARKER for e in outcome["guard_events"])
    print(f"PASS actual supervisor_bundle stdin: fake setting + real Python stdin guard accepted/restored; artifact={tmp}")


def production_authority_unchanged() -> None:
    """Given the test injection, Then SSH payload and target authority are untouched."""
    assert "jetson.local" in " ".join(stage1.SSH)
    assert stage1.EFFECTIVE_CAPS["registered_watches"] == 1000128
    from target_safety.setting import CEILING_SETTING
    assert CEILING_SETTING == "fs.inotify.max_user_watches"
    assert CEILING_ORIGINAL == 65536 and CEILING_TEMPORARY == 1048576
    print("PASS production SSH payload and target authority unchanged by test injection")


def main() -> int:
    bundle_preamble_is_unmodified()
    actual_bundle_stdin_with_fake_setting_and_guard()
    production_authority_unchanged()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
