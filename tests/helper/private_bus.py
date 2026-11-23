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
import re
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


def line(child: subprocess.Popen[bytes], timeout: float = 10, *, stderr: bool = False) -> str:
    stream = child.stderr if stderr else child.stdout
    assert stream is not None
    deadline = time.monotonic() + timeout
    data = bytearray()
    while not data.endswith(b"\n"):
        remaining = deadline - time.monotonic()
        assert remaining > 0 and select.select([stream], [], [], remaining)[0], "process line deadline"
        byte = os.read(stream.fileno(), 1)
        assert byte, f"process closed output: {child.poll()}"
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
         suite: str = "HelperBus.*", helper: subprocess.Popen[bytes] | None = None) -> None:
    with process([str(build / "aa_system_helper_bus_test"), f"--gtest_filter={suite}"], env) as test:
        while test.poll() is None:
            text = line(test)
            if text == "COMMAND wait-entered":
                assert helper is not None
                send(test, line(helper, 2))
            elif text == "COMMAND release-held":
                assert helper is not None
                send(helper, "release")
                send(test, "released")
            elif text.startswith("COMMAND "):
                send(receiver, text.removeprefix("COMMAND "))
                send(test, line(receiver))
            else:
                print(text, flush=True)
            if text.startswith("[  PASSED  ]") or text.startswith("[  FAILED  ]") and "tests, listed below" in text:
                break
        assert test.wait(timeout=10) == 0


def concurrent_flow(build: Path, directory: Path) -> None:
    """Drive same-instance regressions on fresh real buses and an inert backend."""
    env = dict(os.environ, AA_HELPER_HOLD_RELEASE="1")
    fixture = build / "aa_system_helper_fixture"
    with contextlib.ExitStack() as stack:
        buses = [stack.enter_context(process(["dbus-daemon", "--session", "--nofork", "--print-address=1"], env))
                 for _ in range(2)]
        helper_address, env["DBUS_SESSION_BUS_ADDRESS"] = [line(bus) for bus in buses]
        env["AA_HELPER_TEST_BUS"] = helper_address
        helper = stack.enter_context(process([str(fixture), "helper", helper_address, str(directory),
                                              str(directory / "concurrent-effects")], env))
        assert line(helper) == "ready"
        receiver = stack.enter_context(process([str(fixture), "receiver", helper_address,
                                               str(directory / "concurrent-store"), "unused"], env))
        assert line(receiver) == "ready"
        flow(build, receiver, env, env.get("AA_HELPER_CONCURRENCY_FILTER", "HelperConcurrency.*"), helper)


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
    concurrent_flow(build, directory)
    if os.environ.get("AA_HELPER_CONCURRENCY_ONLY") == "1":
        return
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


def unique_names(address: str) -> set[str]:
    output = subprocess.check_output(
        ["dbus-send", f"--bus={address}", "--print-reply", "--dest=org.freedesktop.DBus",
         "/org/freedesktop/DBus", "org.freedesktop.DBus.ListNames"], text=True, timeout=2,
    )
    # ListNames includes the short-lived connection that issued the query.
    query = re.search(r"destination=(:[\d.]+)", output)
    assert query, output
    return set(re.findall(r'string "(:[\d.]+)"', output)) - {query[1]}


def receiver_startup(build: Path, directory: Path) -> None:
    receiver = build / "aa-wireless-receiver"
    fixture = build / "aa_system_helper_fixture"
    env = dict(os.environ, AA_WIRELESS_TEST_SAME_UID_HELPER="1")
    uid = directory / "session.uid"
    uid.write_text(f"{os.getuid()}\n")
    uid.chmod(0o600)
    digest = directory / "receiver.sha256"
    effects = directory / "effects"
    with contextlib.ExitStack() as stack:
        buses = [stack.enter_context(process(["dbus-daemon", "--session", "--nofork",
                                             "--print-address=1"], env)) for _ in range(2)]
        address, env["DBUS_SESSION_BUS_ADDRESS"] = [line(bus) for bus in buses]
        args = [str(receiver), "--store-dir", str(directory), "--pairing-only",
                "--helper-bus-address", address]
        digest.write_text(hashlib.sha256(receiver.read_bytes()).hexdigest() + "\n")
        digest.chmod(0o600)
        helper_args = [str(fixture), "helper", address, str(directory), str(effects)]
        with process(helper_args, env) as helper:
            assert line(helper) == "ready"
            if os.getuid() != 0:
                result = subprocess.run([*args, "--duration-ms", "100"],
                                        env={**env, "AA_WIRELESS_TEST_SAME_UID_HELPER": "0"},
                                        capture_output=True, text=True, timeout=3)
                assert result.returncode == 3 and "AP status: helper_rejected" in result.stderr, result
                assert not effects.exists()
            idle_names = unique_names(address)
            with process([*args, "--duration-ms", "10000"], env) as first:
                ready = line(first, 3)
                assert '"state":"pairing_only"' in ready, ready
                first_name, = unique_names(address) - idle_names
                lookup_args = [str(build / "aa_system_helper_bus_test"), "--gtest_filter=HelperNameOwner.*"]
                lookup_env = dict(env, AA_HELPER_TEST_BUS=address, AA_HELPER_TEST_RECEIVER_NAME=first_name)
                subprocess.run(lookup_args, env=lookup_env, check=True, timeout=3)
                with process([*args, "--duration-ms", "100"], env) as second:
                    # Keep the first receiver alive until the helper sees the second probe.
                    assert "previous receiver still connected" in line(helper, 3, stderr=True)
                    first.terminate()
                    assert first.wait(timeout=3) == 0
                    output, error = second.communicate(timeout=6)
                    assert second.returncode == 0 and b'"state":"pairing_only"' in output, (output, error)
                # Check the adapter directly so the helper's one-second sweep cannot mask a bad answer.
                subprocess.run(lookup_args, env=dict(lookup_env, AA_HELPER_TEST_RECEIVER_EXITED="1"),
                               check=True, timeout=3)
            assert effects.read_text().splitlines().count("effect 4") == 2
        effects.unlink()
        digest.write_text("0" * 64 + "\n")
        with process(helper_args, env) as helper:
            assert line(helper) == "ready"
            result = subprocess.run([*args, "--duration-ms", "100"], env=env,
                                    capture_output=True, text=True, timeout=3)
            assert result.returncode == 3 and "AP status: helper_rejected" in result.stderr, result
            assert "receiver identity verification failed" in line(helper, 1, stderr=True)
            assert not effects.exists()

        digest.write_text(hashlib.sha256(receiver.read_bytes()).hexdigest() + "\n")
        with process(helper_args, dict(env, AA_HELPER_HOLD_STATUS="1")) as helper:
            assert line(helper) == "ready"
            with process(args, env) as client:
                assert line(helper, 3) == "entered"
                buses[0].terminate()
                buses[0].wait(timeout=2)
                # The status request was in flight; EOF must end startup immediately.
                _, error = client.communicate(timeout=2)
                assert client.returncode == 3 and b"AP status: helper_unavailable" in error, error


def main() -> None:
    build = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="helper-", dir=build) as temporary:
        if sys.argv[2:] == ["receiver-startup"]:
            receiver_startup(build, Path(temporary))
        else:
            run(build, Path(temporary))


if __name__ == "__main__":
    main()
