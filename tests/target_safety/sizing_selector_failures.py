# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_selector_failures.py
"""Selector wait-bound and owned-resource cleanup fault-boundary regressions.

Deterministic clock/selector scripting plus typed injected faults over real
local children and real pipes. Every injected fault is labeled 'injected ...'
and proves closure or fail-closed contracts only; it is never presented as an
organic Jetson observation. No kernel mutation and no target contact. The
scripted clock/selector fixtures live in sizing_collector and are shared.
"""
from __future__ import annotations

import selectors
import sys
import time
from pathlib import Path
from typing import IO
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import collector
from target_safety.collector import CollectionPolicy, Collector
from sizing_collector import (CloseFaultStream, ExitedProcView, FakeClock, FaultCloseView,
                              FaultLabels, ScriptedSelector, open_fds, spawn_child)


class _UnexpectedRegisterSelector(ScriptedSelector):
    """Selector whose register raises a non-IO error (injected unexpected)."""

    def register(self, fileobj: IO[bytes] | int, events: int, data: object = None) -> selectors.SelectorKey:
        raise ValueError("injected unexpected register failure (scripted)")


def sampled_wait_honors_interval_remainder_after_late_event() -> None:
    # Given: sampling thresholds unchanged and a 60 ms readiness event mid-interval.
    assert collector.SAMPLE_INTERVAL_SECONDS == 0.1
    policy = CollectionPolicy()
    assert (policy.sample_interval, policy.combined_limit,
            policy.record_limit, policy.drain_grace) == (0.1, 16777216, 65536, 5.0)
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [(1.060, ("out",))], FaultLabels())
    calls: list[float] = []

    def sample() -> str | None:
        calls.append(clock.now)
        return "resource_abort fixture stop" if len(calls) > 1 else None

    proc = spawn_child("import sys, time; sys.stdout.write('x'); sys.stdout.flush(); time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, None, clock.now + 10.0, sample=sample).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: the wait after the event is the interval remainder, not a fresh 100 ms.
    assert calls[0] == 1.000 and calls[1] <= 1.100 + 1e-9, calls
    assert script.timeouts[1] <= 0.040 + 1e-9, script.timeouts
    assert result.invalidated and result.samples == 2
    print(f"PASS sampled wait honored the interval remainder (sampled 1.000 then {calls[1]:.3f})")


def wait_is_bounded_by_collection_deadline() -> None:
    # Given: a collection deadline nearer than the next due sample.
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [], FaultLabels())
    proc = spawn_child("import time; time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, None, 1.05, sample=lambda: None).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: the selector wait never exceeds the remaining collection time.
    assert script.timeouts[0] <= 0.05 + 1e-9, script.timeouts
    assert result.timed_out and result.first_cause == "collection_deadline", result
    print("PASS selector wait bounded by the remaining collection deadline (0.05 s)")


def drain_budget_bounds_wait_after_child_exit() -> None:
    # Given: a scripted exited child whose pipe never reports EOF (descendant holds it).
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [(1.001, ("out",))], FaultLabels())
    proc = spawn_child("import sys, time; sys.stdout.write('x'); sys.stdout.flush(); time.sleep(30)")
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(ExitedProcView(proc), None, clock.now + 10.0,
                               policy=CollectionPolicy(drain_grace=0.05)).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: while draining, the wait never exceeds the remaining drain budget.
    assert result.first_cause == "drain_incomplete", result.first_cause
    assert script.timeouts[1] <= 0.05 + 1e-9, script.timeouts
    print("PASS drain budget bounded the selector wait after the scripted child exit")


def register_fault_invalidates_and_closes_selector_and_pipes() -> None:
    # Given: a real child on real pipes whose first registration faults (injected ENOMEM).
    script = ScriptedSelector(FakeClock(1.000), [],
                              FaultLabels(register="injected register ENOMEM (scripted)"))
    proc = spawn_child("import time; time.sleep(30)")
    pipes = {proc.stdin.fileno(), proc.stdout.fileno(), proc.stderr.fileno()}
    try:
        caught: Exception | None = None
        result = None
        try:
            with patch("selectors.DefaultSelector", lambda: script):
                result = Collector(proc, b"x", time.monotonic() + 5.0).run()
        except Exception as error:
            caught = error
        # Then: the expected IO setup fault is a rejected result, and nothing is held.
        assert caught is None, f"run() raised {caught!r} instead of rejecting; held={pipes & set(open_fds())}"
        assert result is not None and result.invalidated
        assert result.first_cause and "selector_register_fault" in result.first_cause
        assert "injected" in result.first_cause
        assert not pipes & set(open_fds()), f"pipe fds held: {pipes & set(open_fds())}"
        assert script.closed
        print("PASS register ENOMEM (injected) rejects and closes selector and every original pipe")
    finally:
        proc.kill()
        proc.wait(timeout=5)


def selector_construction_fault_invalidates_and_closes_pipes() -> None:
    # Given: a real child on real pipes whose selector construction faults (injected EMFILE).
    proc = spawn_child("import time; time.sleep(30)")
    pipes = {proc.stdin.fileno(), proc.stdout.fileno(), proc.stderr.fileno()}

    def _raising_factory() -> selectors.BaseSelector:
        raise OSError("injected selector construction failure (scripted)")

    try:
        caught: Exception | None = None
        result = None
        try:
            with patch("selectors.DefaultSelector", _raising_factory):
                result = Collector(proc, b"x", time.monotonic() + 5.0).run()
        except Exception as error:
            caught = error
        # Then: the expected construction fault is a rejected result; pipes still close.
        assert caught is None, f"run() raised {caught!r} instead of rejecting; held={pipes & set(open_fds())}"
        assert result is not None and result.invalidated
        assert result.first_cause and "selector_construction_fault" in result.first_cause
        assert "injected" in result.first_cause
        assert not pipes & set(open_fds()), f"pipe fds held: {pipes & set(open_fds())}"
        print("PASS construction IO fault (injected) rejects and closes every original pipe")
    finally:
        proc.kill()
        proc.wait(timeout=5)


def select_fault_invalidates_and_closes_all_resources() -> None:
    # Given: a real child on real pipes whose selector wait faults (injected EIO).
    script = ScriptedSelector(FakeClock(1.000), [],
                              FaultLabels(select="injected select EIO (scripted)"))
    proc = spawn_child("import time; time.sleep(30)")
    pipes = {proc.stdin.fileno(), proc.stdout.fileno(), proc.stderr.fileno()}
    try:
        caught: Exception | None = None
        result = None
        try:
            with patch("selectors.DefaultSelector", lambda: script):
                result = Collector(proc, b"x", time.monotonic() + 5.0).run()
        except Exception as error:
            caught = error
        # Then: the select fault rejects the result and closure is still complete.
        assert caught is None, f"run() raised {caught!r} instead of rejecting; held={pipes & set(open_fds())}"
        assert result is not None and result.invalidated
        assert result.first_cause and "selector_select_fault" in result.first_cause
        assert "injected" in result.first_cause
        assert not pipes & set(open_fds()), f"pipe fds held: {pipes & set(open_fds())}"
        assert script.closed
        print("PASS select IO fault (injected) rejects and closes selector and every original pipe")
    finally:
        proc.kill()
        proc.wait(timeout=5)


def cleanup_unregister_fault_still_closes_selector_and_pipes() -> None:
    # Given: resource abort reached and cleanup unregistration faulting (injected EIO).
    script = ScriptedSelector(FakeClock(1.000), [],
                              FaultLabels(unregister="injected cleanup unregister EIO (scripted)"))
    proc = spawn_child("import time; time.sleep(30)")
    pipes = {proc.stdin.fileno(), proc.stdout.fileno(), proc.stderr.fileno()}
    try:
        caught: Exception | None = None
        result = None
        try:
            with patch("selectors.DefaultSelector", lambda: script):
                result = Collector(proc, b"x", time.monotonic() + 5.0,
                                   sample=lambda: "resource_abort fixture stop").run()
        except Exception as error:
            caught = error
        # Then: the cleanup fault cannot skip the selector close or any remaining pipe close.
        assert caught is None, f"run() raised {caught!r} instead of rejecting; held={pipes & set(open_fds())}"
        assert result is not None and result.invalidated
        assert "resource_abort" in (result.first_cause or ""), result.first_cause
        assert any("selector_unregister_fault" in cause for cause in result.violation_details), result
        assert not pipes & set(open_fds()), f"pipe fds held: {pipes & set(open_fds())}"
        assert script.closed
        print("PASS cleanup unregister EIO (injected) never skips selector or remaining pipe closes")
    finally:
        proc.kill()
        proc.wait(timeout=5)


def close_faults_record_but_remaining_closers_still_run() -> None:
    # Given: a real child on real pipes whose stdout and stdin closes fault after release.
    proc = spawn_child("import sys; sys.stdout.write('x')")
    out_fd, err_fd, in_fd = proc.stdout.fileno(), proc.stderr.fileno(), proc.stdin.fileno()
    result = Collector(FaultCloseView(proc), b"x", time.monotonic() + 5.0).run()
    proc.wait(timeout=5)
    # Then: the close faults are recorded and every remaining closer still runs.
    assert result.invalidated and any("close_fault" in cause for cause in result.violation_details), result
    assert not {out_fd, err_fd, in_fd} & set(open_fds()), open_fds()
    assert not any("eventpoll" in target for target in open_fds().values())
    print("PASS close faults recorded and every remaining pipe and selector still closed")


def unexpected_setup_fault_propagates_after_pipes_close() -> None:
    # Given: a real child on real pipes whose registration raises a non-IO error.
    script = _UnexpectedRegisterSelector(FakeClock(1.000), [], FaultLabels())
    proc = spawn_child("import time; time.sleep(30)")
    pipes = {proc.stdin.fileno(), proc.stdout.fileno(), proc.stderr.fileno()}
    try:
        caught: ValueError | None = None
        try:
            with patch("selectors.DefaultSelector", lambda: script):
                Collector(proc, b"x", time.monotonic() + 5.0).run()
        except ValueError as error:
            caught = error
        # Then: the unexpected error propagates only after closure while the traceback is held.
        assert caught is not None and caught.__traceback__ is not None
        assert "injected" in str(caught)
        assert not pipes & set(open_fds()), f"pipe fds held while traceback alive: {pipes & set(open_fds())}"
        print("PASS unexpected setup failure propagates only after every pipe closed (traceback held)")
    finally:
        proc.kill()
        proc.wait(timeout=5)


def fault_flood_retention_stays_bounded_with_first_cause() -> None:
    # Given: persistent injected read and unregister faults flooding causes per wake.
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [],
                              FaultLabels(unregister="injected unregister EIO (scripted)"),
                              sticky_out=True)

    def _injected_read_fault(fd: int, size: int) -> bytes:
        raise OSError("injected persistent read fault (scripted)")

    proc = spawn_child("import time; time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script), \
                patch("os.read", _injected_read_fault):
            result = Collector(proc, None, clock.now + 2.0).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: retention stays capped while the first cause survives the flood.
    assert result.invalidated and result.timed_out
    assert result.first_cause == "stream_read_fault injected persistent read fault (scripted)", result.first_cause
    assert any("selector_unregister_fault" in cause for cause in result.violation_details), result.violation_details
    assert len(result.violation_details) <= collector.MAX_VIOLATION_DETAILS, result.violation_details
    print(f"PASS fault flood retained {len(result.violation_details)} capped details with the first cause intact")


def main() -> int:
    tests = (
        sampled_wait_honors_interval_remainder_after_late_event,
        wait_is_bounded_by_collection_deadline,
        drain_budget_bounds_wait_after_child_exit,
        register_fault_invalidates_and_closes_selector_and_pipes,
        selector_construction_fault_invalidates_and_closes_pipes,
        select_fault_invalidates_and_closes_all_resources,
        cleanup_unregister_fault_still_closes_selector_and_pipes,
        close_faults_record_but_remaining_closers_still_run,
        unexpected_setup_fault_propagates_after_pipes_close,
        fault_flood_retention_stays_bounded_with_first_cause,
    )
    failed: list[str] = []
    for test in tests:
        try:
            test()
        except Exception as error:
            failed.append(test.__name__)
            print(f"FAIL {test.__name__}: {type(error).__name__}: {error}")
    print(f"{len(tests) - len(failed)}/{len(tests)} selector failure-boundary tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
