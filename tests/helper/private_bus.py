# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/private_bus.py build/host-dev
"""Host-only two-bus process acceptance; every writable fixture stays in build."""
from __future__ import annotations

import contextlib
import hashlib
import os
from pathlib import Path
import select
import subprocess
import sys
import tempfile
import time
from collections.abc import Iterator


@contextlib.contextmanager
def process(args: list[str], env: dict[str, str]) -> Iterator[subprocess.Popen[bytes]]:
    with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=env, bufsize=0) as child:
        try:
            yield child
        finally:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)


def line(child: subprocess.Popen[bytes], timeout: float = 10) -> str:
    assert child.stdout is not None
    deadline = time.monotonic() + timeout
    data = bytearray()
    while not data.endswith(b"\n"):
        remaining = deadline - time.monotonic()
        assert remaining > 0 and select.select([child.stdout], [], [], remaining)[0], "process line deadline"
        byte = os.read(child.stdout.fileno(), 1)
        assert byte, f"process closed stdout: {child.poll()}"
        data.extend(byte)
        assert len(data) <= 65536
    return data.decode().rstrip("\n")


def send(child: subprocess.Popen[bytes], text: str) -> None:
    assert child.stdin is not None
    child.stdin.write((text + "\n").encode())
    child.stdin.flush()


def rss(child: subprocess.Popen[bytes]) -> int:
    fields = Path(f"/proc/{child.pid}/status").read_text().splitlines()
    return int(next(field for field in fields if field.startswith("VmRSS:")).split()[1]) * 1024


def flow(build: Path, receiver: subprocess.Popen[bytes], env: dict[str, str],
         suite: str = "HelperBus.*") -> None:
    with process([str(build / "aa_system_helper_bus_test"), f"--gtest_filter={suite}"], env) as test:
        while test.poll() is None:
            text = line(test)
            if text.startswith("COMMAND "):
                send(receiver, text.removeprefix("COMMAND "))
                send(test, line(receiver))
            else:
                print(text, flush=True)
            if text.startswith("[  PASSED  ]") or text.startswith("[  FAILED  ]") and "tests, listed below" in text:
                break
        assert test.wait(timeout=10) == 0


def wait_cleanup(path: Path, previous: int, start: float) -> float:
    while time.monotonic() - start < 10:
        if path.read_text().count("cleanup") > previous:
            return time.monotonic() - start
        select.select([], [], [], 0.02)
    raise AssertionError("receiver-loss cleanup exceeded 10 seconds")


def run(build: Path, directory: Path) -> None:
    env = dict(os.environ)
    fixture = build / "aa_system_helper_fixture"
    (directory / "session.uid").write_text(f"{os.getuid()}\n")
    (directory / "receiver.sha256").write_text(hashlib.sha256(fixture.read_bytes()).hexdigest() + "\n")
    for name in ("session.uid", "receiver.sha256"):
        (directory / name).chmod(0o600)
    effects = directory / "effects"
    effects.touch()
    with contextlib.ExitStack() as stack:
        buses = [stack.enter_context(process(["dbus-daemon", "--session", "--nofork", "--print-address=1",
                   f"--address=unix:abstract=aa34-{os.getpid()}-{index}"], env)) for index in range(2)]
        helper_address, session_address = [line(bus) for bus in buses]
        env["AA_HELPER_TEST_BUS"] = helper_address
        env["DBUS_SESSION_BUS_ADDRESS"] = session_address
        helper = stack.enter_context(process([str(fixture), "helper", helper_address, str(directory), str(effects)], env))
        assert line(helper) == "ready"
        peak = rss(helper)
        for index in range(2):
            receiver = stack.enter_context(process([str(fixture), "receiver", helper_address,
                       str(directory / f"store-{index}"), "unused"], env))
            assert line(receiver) == "ready"
            flow(build, receiver, env)
            send(receiver, "refresh")
            assert line(receiver) == "0"
            send(receiver, "reconnect")
            assert line(receiver) == "0"
            peak = max(peak, rss(helper))
            previous = effects.read_text().count("cleanup")
            if index == 0:
                flood = stack.enter_context(process([str(build / "aa_system_helper_bus_test"),
                        "--gtest_filter=HelperFlood.*"], env))
                while line(flood) != "flood-ready":
                    pass
                started = time.monotonic()
                receiver.kill()
                receiver.wait(timeout=5)
                elapsed = wait_cleanup(effects, previous, started)
                assert flood.wait(timeout=10) == 0
                print(f"receiver_crash_under_flood_cleanup_seconds={elapsed:.3f}")
            else:
                started = time.monotonic()
                buses[0].terminate()
                buses[0].wait(timeout=5)
                elapsed = wait_cleanup(effects, previous, started)
                assert helper.wait(timeout=5) == 5
                receiver.kill()
                receiver.wait(timeout=5)
                print(f"bus_loss_cleanup_seconds={elapsed:.3f}")
        assert peak <= 32 * 1024 * 1024, f"fixture daemon RSS {peak} exceeds 32 MiB"
        print(f"recording_helper_peak_rss_bytes={peak}")
        production_bus = stack.enter_context(process(["dbus-daemon", "--session", "--nofork", "--print-address=1",
                            f"--address=unix:abstract=aa34-{os.getpid()}-production"], env))
        address = line(production_bus)
        daemon = stack.enter_context(process([str(build / "aa-system-helper"), "--private-bus", address, str(directory)], env))
        select.select([], [], [], 0.1)
        assert daemon.poll() is None
        production_peak = rss(daemon)
        # A real receiver issues bounded status traffic to the production binary;
        # privileged effects stay unavailable even with a valid confirmation.
        production_receiver = stack.enter_context(process([str(fixture), "receiver", address,
                              str(directory / "production-store"), "unused"], env))
        assert line(production_receiver) == "ready"
        flow(build, production_receiver, env, "HelperProduction.*")
        for _ in range(100):
            send(production_receiver, "status")
            assert line(production_receiver) == "0"
            production_peak = max(production_peak, rss(daemon))
        assert production_peak <= 32 * 1024 * 1024
        print(f"production_helper_peak_rss_bytes={production_peak}")


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="helper-", dir=build) as temporary:
        run(build, Path(temporary))


if __name__ == "__main__":
    main()
