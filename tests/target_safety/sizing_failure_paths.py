# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_failure_paths.py
"""Failure-path regressions: guard checkpoint attribution and collector cleanup.

Real local children, real pipes/pty/selector and real zombie /proc readings;
setting authority stays a local fake. Expected boundary failures invalidate the
collection and the window: never accepted, never silent and never left open.
"""
from __future__ import annotations

import io
import json
import os
import subprocess
import sys
import tempfile
import time
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import resource
from target_safety.collector import Collector
from target_safety.report import accepted_ceiling_output
from target_safety.resource import ResourceError, Sampler
from target_safety.supervisor import Supervisor
from ceiling_fixtures import guard_argv
from ceiling_setting_fixtures import fake_setting

GIB = 1024 ** 3
MIB = 2 ** 20
PY = (sys.executable, "-B", "-c")


class _StreamFault:
    """Real pipe stream view whose close() faults once; fd access delegates."""

    def __init__(self, stream: object) -> None:
        self._stream = stream

    def fileno(self) -> int:
        return self._stream.fileno()

    def close(self) -> None:
        raise OSError("injected close fault")


class _ProcView:
    """Real Popen view with one stream replaced; poll delegates to the real child."""

    def __init__(self, proc: subprocess.Popen[bytes], stdout: object) -> None:
        self._proc = proc
        self.stdout = stdout
        self.stderr = proc.stderr
        self.stdin = proc.stdin

    def poll(self) -> int | None:
        return self._proc.poll()


def meminfo(bytes_available: int) -> Path:
    root = Path(tempfile.mkdtemp(prefix="failure-meminfo-", dir="/tmp/opencode"))
    path = root / "meminfo"
    path.write_text(f"MemTotal: 1 kB\nMemAvailable:  {bytes_available // 1024} kB\n")
    return path


def _pump(source: str, stdin: bytes | None, sample=None):
    proc = subprocess.Popen(PY + (source,), stdin=subprocess.PIPE if stdin is not None else subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        return Collector(proc, stdin, time.monotonic() + 10.0, sample=sample).run()
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)


def _open_fds() -> dict[int, str]:
    """Snapshot open fd targets; the snapshot's own transient fd is skipped.

    Listing /proc/self/fd briefly exposes the listing machinery's own fd which
    is gone at readlink time; a genuinely held fd persists and always resolves.
    """
    dirfd = os.open("/proc/self/fd", os.O_RDONLY | os.O_DIRECTORY)
    try:
        fds = {int(fd) for fd in os.listdir(dirfd)} - {dirfd}
    finally:
        os.close(dirfd)
    targets: dict[int, str] = {}
    for fd in sorted(fds):
        try:
            targets[fd] = os.readlink(f"/proc/self/fd/{fd}")
        except FileNotFoundError:
            continue
    return targets


def guard_checkpoint_attributes_own_rss_to_guard_threshold_only() -> None:
    # Given: guard RSS between the 128 MiB supervisor and 1 GiB guard thresholds.
    reads: list[int] = []
    def fake_rss(pid: int, proc: Path | None = None) -> int:
        assert pid == os.getpid(), f"non-self pid read: {pid}"
        reads.append(pid)
        return 256 * MIB
    with patch.object(resource, "read_rss_bytes", fake_rss):
        resource.checkpoint(meminfo(4 * GIB))
    # Then: it passes and only the guard's own RSS is read, once.
    assert len(reads) == 1, reads
    print("PASS guard checkpoint 256 MiB passes on the 1 GiB guard threshold; self RSS read once")


def guard_checkpoint_exact_ceiling_passes_and_above_aborts() -> None:
    def at(value: int) -> None:
        with patch.object(resource, "read_rss_bytes", lambda pid, proc=None: value):
            resource.checkpoint(meminfo(4 * GIB))
    # Given: guard RSS exactly at its 1 GiB ceiling. Then: the trigger is strictly above.
    at(GIB)
    try:
        at(GIB + 1024)
    except ResourceError as error:
        assert error.operation == "guard_resource_abort" and "guard_rss" in error.detail, error
    else:
        raise AssertionError("guard RSS above the guard ceiling accepted")
    print("PASS guard checkpoint exact 1 GiB passes and above 1 GiB typed-aborts")


def supervisor_sampler_keeps_pid_thresholds_separate() -> None:
    # Given: fixture /proc readings for the guard child and the supervisor pids.
    root = Path(tempfile.mkdtemp(prefix="failure-proc-", dir="/tmp/opencode"))
    for pid in (os.getpid(), 4242):
        (root / str(pid)).mkdir()
    def status(pid: int, kib: int) -> None:
        (root / str(pid) / "status").write_text(f"Name:\tfixture\nVmRSS:\t{kib} kB\n")
    sampler = Sampler(4242, meminfo=meminfo(4 * GIB), proc=root)
    status(4242, 256 * 1024)
    status(os.getpid(), 50 * 1024)
    assert sampler.sample() is None
    status(os.getpid(), 256 * 1024)
    # Then: the 128 MiB supervisor bound fires on the supervisor pid only.
    assert "supervisor_rss" in (sampler.sample() or "")
    status(4242, 2 * GIB // 1024)
    status(os.getpid(), 50 * 1024)
    # Then: the 1 GiB guard bound fires on the child pid.
    assert "guard_rss" in (sampler.sample() or "")
    print("PASS supervisor sampler keeps 128 MiB supervisor and 1 GiB guard bounds on separate pids")


def sample_failure_returns_invalidated_result_with_evidence() -> None:
    # Given: a real child on real pipes whose resource sample fails typed after Popen.
    calls: list[int] = []
    def failing_sample() -> str | None:
        calls.append(1)
        if len(calls) > 1:
            raise ResourceError("rss_unavailable", "/proc/fixture: simulated missing child proc")
        return None
    result = _pump("import sys, time\nsys.stdout.write('kept'); sys.stdout.flush(); time.sleep(30)",
                   None, sample=failing_sample)
    # Then: the typed failure becomes an invalidated result keeping bytes/hash/cause.
    assert result.invalidated and result.stdout == b"kept", result
    assert "rss_unavailable" in (result.first_cause or ""), result.first_cause
    assert len(result.stdout_sha256) == 64 and len(result.violation_details) <= 8
    print("PASS sample ResourceError returns invalidated result with bytes/hash/first cause retained")


def unexpected_error_still_closes_all_resources() -> None:
    # Given: a real child whose sample seam raises a non-typed error.
    proc = subprocess.Popen(PY + ("import time; time.sleep(30)",), stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    pipes = {proc.stdin.fileno(), proc.stdout.fileno(), proc.stderr.fileno()}
    def boom() -> str:
        raise RuntimeError("unexpected instrumented failure")
    caught: RuntimeError | None = None
    try:
        Collector(proc, b"x", time.monotonic() + 5.0, sample=boom).run()
    except RuntimeError as error:
        caught = error
    # Then: the error propagates but every resource is closed while the traceback is held.
    assert caught is not None and caught.__traceback__ is not None
    targets = _open_fds()
    assert not pipes & set(targets), f"pipe fds held while traceback alive: {pipes & set(targets)}"
    assert not any("eventpoll" in target for target in targets.values())
    proc.kill()
    proc.wait(timeout=5)
    print("PASS unexpected collector failure propagates with selector and pipes already closed")


def zombie_and_malformed_readings_fail_closed() -> None:
    # Given: a real unreaped zombie child with no VmRSS line in /proc.
    child = subprocess.Popen(PY + ("pass",))
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if Path(f"/proc/{child.pid}/stat").read_text().rsplit(")", 1)[1].split()[0] == "Z":
            break
        time.sleep(0.01)
    else:
        raise AssertionError("fixture child never became a zombie")
    try:
        resource.read_rss_bytes(child.pid)
    except ResourceError as error:
        assert error.operation == "rss_malformed", error
    else:
        raise AssertionError("zombie reading accepted")
    finally:
        child.wait(timeout=5)
    root = Path(tempfile.mkdtemp(prefix="failure-status-", dir="/tmp/opencode"))
    (root / "7").mkdir()
    (root / "7" / "status").write_text("Name:\tfixture\nVmRSS:\tnot-a-number kB\n")
    try:
        resource.read_rss_bytes(7, root)
    except ResourceError as error:
        assert error.operation == "rss_malformed", error
    else:
        raise AssertionError("malformed reading accepted")
    print("PASS zombie and malformed required readings fail closed as typed failures")


def read_fault_is_recorded_and_never_accepted_as_eof() -> None:
    # Given: a real child on a real pty whose master read faults EIO after exit.
    master, slave = os.openpty()
    child = subprocess.Popen(PY + ("import sys; sys.stdout.write('data'); sys.stdout.flush()",),
                             stdin=subprocess.DEVNULL, stdout=slave, stderr=subprocess.DEVNULL)
    os.close(slave)
    result = Collector(_ProcView(child, os.fdopen(master, "rb")), None,
                       time.monotonic() + 5.0).run()
    child.wait(timeout=5)
    # Then: the organic read fault is recorded; EOF is never faked into success.
    assert result.stdout == b"data" and result.invalidated, result
    assert "read" in (result.first_cause or ""), result.first_cause
    print("PASS organic pty read fault recorded; an IO error is never accepted as EOF")


def write_fault_invalidates_incomplete_stdin() -> None:
    # Given: a real child that closes stdin without consuming the payload.
    result = _pump("import os, sys\nos.close(0)\nsys.stdout.write('done')", b"x" * 500_000)
    # Then: the write fault invalidates; incomplete stdin is never fake success.
    assert result.stdin_complete is False and result.stdout == b"done", result
    assert result.invalidated and "stdin" in (result.first_cause or ""), result.first_cause
    print("PASS stdin write fault recorded; incomplete stdin never fakes final success")


def close_fault_is_recorded_and_other_streams_still_close() -> None:
    # Given: a real child whose stdout stream view faults on close only.
    proc = subprocess.Popen(PY + ("import sys; sys.stdout.write('x')",), stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    err_fd = proc.stderr.fileno()
    result = Collector(_ProcView(proc, _StreamFault(proc.stdout)), None,
                       time.monotonic() + 5.0).run()
    proc.wait(timeout=5)
    # Then: the close fault is recorded and the remaining resources are still closed.
    assert result.invalidated and any("close_fault" in cause for cause in result.violation_details), result
    assert proc.stderr.closed
    targets = _open_fds()
    assert err_fd not in targets and not any("eventpoll" in target for target in targets.values()), targets
    print("PASS close fault recorded and the remaining streams and selector still close")


def window_with_sample_failure_returns_rejected_wire() -> None:
    # Given: real window, fake authority, real guard child, child /proc failing after Popen.
    real_rss = resource.read_rss_bytes
    def missing_child_proc(pid: int, proc: Path = resource.PROC_STATUS) -> int:
        if pid != os.getpid():
            raise ResourceError("rss_unavailable", f"/proc/{pid}/status: simulated missing child proc")
        return real_rss(pid, proc)
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        supervisor = Supervisor(setting, guard_argv(root, "hang"))
        out = io.StringIO()
        with patch.object(resource, "read_rss_bytes", missing_child_proc), redirect_stdout(out):
            outcome = supervisor.run()
        # Then: structured rejected outcome with restoration record, wire and reaped guard.
        assert outcome.accepted is False and outcome.guard_valid is False, outcome
        assert outcome.restored_original and outcome.restore_class == "restore_original"
        assert outcome.original_observed == 65536 and outcome.temp_observed == 1048576
        assert outcome.restore_observed == 65536 and state.read_text() == "65536"
        assert outcome.collection is not None and outcome.collection["invalidated"]
        assert "rss_unavailable" in (outcome.collection["first_cause"] or "")
        guard = supervisor.guard
        assert guard is not None and guard.terminated
        assert guard.proc is not None and guard.proc.returncode is not None
        wires = [json.loads(line) for line in out.getvalue().splitlines()]
        emitted = [wire for wire in wires if wire["event"] == "ceiling_outcome"]
        assert len(emitted) == 1 and emitted[0]["data"]["accepted"] is False, out.getvalue()
        assert emitted[0]["data"]["restore_observed"] == 65536
        assert accepted_ceiling_output(out.getvalue()) is False
    print("PASS window sample failure returns structured rejected wire with restore record and reaped guard")


def main() -> int:
    guard_checkpoint_attributes_own_rss_to_guard_threshold_only()
    guard_checkpoint_exact_ceiling_passes_and_above_aborts()
    supervisor_sampler_keeps_pid_thresholds_separate()
    sample_failure_returns_invalidated_result_with_evidence()
    unexpected_error_still_closes_all_resources()
    zombie_and_malformed_readings_fail_closed()
    read_fault_is_recorded_and_never_accepted_as_eof()
    write_fault_invalidates_incomplete_stdin()
    close_fault_is_recorded_and_other_streams_still_close()
    window_with_sample_failure_returns_rejected_wire()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
