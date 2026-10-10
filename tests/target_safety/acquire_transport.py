# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_transport.py
"""Fake local transport exercising the exact entry/payload/archive path, bounded
fail-closed output, and the hardened (BatchMode, no-retry) SSH envelope."""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.acquire_transport import (
    TransportConfig,
    _run_bounded,
    build_payload,
    remote_command,
    run_window,
    transport_argv,
)
from target_safety.kernel import Blocked
from acquire_fixture_transport import fixture_transport


def _fixture(base: Path) -> tuple[TransportConfig, Path]:
    root0 = base / "proj"
    (root0 / ".venv/bin").mkdir(parents=True)
    (root0 / "a.txt").write_bytes(b"repo\n")
    os.symlink("/usr/bin/python3", root0 / ".venv/bin/python3")
    root1 = base / "cg"
    root1.mkdir()
    (root1 / "b.txt").write_bytes(b"cg\n")
    tgt = base / "tgt"
    (tgt / "usr/include").mkdir(parents=True)
    (tgt / "usr/lib").mkdir(parents=True)
    (tgt / "usr/include/h.h").write_bytes(b"hdr\n")
    (tgt / "usr/lib/libf.so.1").write_bytes(b"libbytes\n")
    os.symlink("libf.so.1", tgt / "usr/lib/libf.so")
    inputs = [str(tgt / "usr/include/h.h"), str(tgt / "usr/lib/libf.so.1"), str(tgt / "usr/lib/libf.so")]
    pkg = {"name": "libfixture", "version": "1.0-1", "architecture": "arm64", "source": "src"}
    observation = json.dumps({
        "schema": "aa-acquire-observation/1",
        "identity": {"kernel": "6.8.12", "l4t": "R39.2.1", "jetpack": "5.1", "dpkg_summary": []},
        "packages": {p: pkg for p in inputs},
        "inputs": [{"path": p} for p in inputs],
    }).encode()
    cfg = TransportConfig(
        roots=(str(root0), str(root1)),
        link_exceptions=(f"{root0}/.venv/bin/python3=/usr/bin/python3",),
        window_id="win-transport", target_identity="jetson.local",
        remote_scratch=str(base / "remote"), local_out=base / "local",
        observation=observation,
    )
    return cfg, tgt


def test_fake_local_transport_e2e() -> None:
    base = Path(tempfile.mkdtemp(prefix="tp-e2e-", dir="/tmp/opencode"))
    cfg, _tgt = _fixture(base)
    with fixture_transport(base):
        result = run_window(cfg, kind="local")
    verification = result["verification"]
    assert verification["equivalent"] is True, verification
    assert result["receipt"]["exit_status"] == 0
    returned = Path(result["returned"])
    for name in ("before.json", "after.json", "diff.json", "receipt.json", "acquisition.json"):
        assert (returned / name).exists(), name
    assert (returned / "snapshot").is_dir()
    copied = sorted(p.relative_to(returned / "snapshot").as_posix()
                    for p in (returned / "snapshot").rglob("*") if p.is_file() or p.is_symlink())
    assert any(p.endswith("usr/include/h.h") for p in copied), copied
    record = json.loads((returned / "acquisition.json").read_text())
    assert record["provenance"] == "fixture"
    print("PASS fake local transport returns bounded archive with evidence + rootfs")


def test_bounded_output_fails_closed() -> None:
    sink = Path(tempfile.mkdtemp(prefix="tp-cap-", dir="/tmp/opencode")) / "out.bin"
    try:
        _run_bounded(("bash", "-c", "head -c 2000000 /dev/zero"), b"", sink, 500000, 10)
    except Blocked as error:
        assert error.reason == "transport_output_bound", error
    else:
        raise AssertionError("streaming output bound must fail closed")
    print("PASS return archive streaming bound fails closed when exceeded")


def test_transport_argv_is_hardened() -> None:
    base = Path(tempfile.mkdtemp(prefix="tp-argv-", dir="/tmp/opencode"))
    cfg, _tgt = _fixture(base)
    argv = transport_argv(cfg, "ssh", 2400)
    joined = " ".join(argv)
    assert argv[:4] == ("timeout", "--signal=TERM", "--kill-after=5s", "2400s")
    assert "ssh" in argv and "BatchMode=yes" in joined and "ConnectionAttempts=1" in joined
    assert argv[-2] == "jetson.local" and "target_safety.acquire" in argv[-1]
    local = transport_argv(cfg, "local", 2400)
    assert local[-3] == "bash" and local[-2] == "-c" and "target_safety.acquire" in local[-1]
    print("PASS SSH envelope is BatchMode + no-retry + bounded; local fake shares entry")


def test_build_payload_includes_input_policy() -> None:
    payload = build_payload(None)
    with tarfile.open(fileobj=__import__("io").BytesIO(payload)) as tar:
        names = tar.getnames()
    assert "target_safety/acquire-input-policy.json" in names
    assert "target_safety/acquire.py" in names and "target_safety/acquire_input_select.py" in names
    assert "observation.json" not in names
    with_obs = build_payload(b'{"schema":"x"}')
    with tarfile.open(fileobj=__import__("io").BytesIO(with_obs)) as tar:
        assert "observation.json" in tar.getnames()
    print("PASS stdin tar payload carries the frozen input policy (+ optional observation)")


def main() -> int:
    test_fake_local_transport_e2e()
    test_bounded_output_fails_closed()
    test_transport_argv_is_hardened()
    test_build_payload_includes_input_policy()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
