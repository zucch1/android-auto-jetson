# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_callback_timing.py
"""Selector wait-remainder clock regressions for costly sampling callbacks.

Deterministic clock/selector scripting over real local children and real pipes.
A scripted bounded reading cost inside the sample() callback must be deducted
from every selector wait remainder (next due sample, collection deadline, drain
budget): each remainder is computed at select start after the callback returns,
never from the stale loop-top clock and never as a fresh full interval. No
kernel mutation and no target contact. The scripted clock/selector fixtures live
in sizing_collector and are shared.
"""
from __future__ import annotations

import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import collector
from target_safety.collector import CollectionPolicy, Collector
from sizing_collector import FakeClock, FaultLabels, ScriptedSelector, open_fds, spawn_child


def sample_cost_is_deducted_from_next_sample_wait() -> None:
    # Given: unchanged sampling thresholds and a scripted 60 ms callback reading cost.
    assert collector.SAMPLE_INTERVAL_SECONDS == 0.1
    policy = CollectionPolicy()
    assert (policy.sample_interval, policy.combined_limit,
            policy.record_limit, policy.drain_grace) == (0.1, 16777216, 65536, 5.0)
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [], FaultLabels())
    calls: list[float] = []

    def sample() -> str | None:
        calls.append(clock.now)
        clock.set(clock.now + 0.060)
        return "resource_abort fixture stop" if len(calls) > 1 else None

    proc = spawn_child("import time; time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, None, clock.now + 10.0, sample=sample).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: the wait is the interval remainder minus the cost and the next sample lands at 1.100.
    assert calls[0] == 1.000 and abs(calls[1] - 1.100) <= 1e-9, calls
    assert abs(script.timeouts[0] - 0.040) <= 1e-9, script.timeouts
    assert result.samples == 2 and result.invalidated, result
    assert "resource_abort" in (result.first_cause or ""), result.first_cause
    print(f"PASS 60 ms callback cost deducted: wait 0.040 and next sample at {calls[1]:.3f}")


def callback_cost_past_deadline_yields_zero_wait() -> None:
    # Given: a 50 ms collection remainder and a 60 ms callback cost consuming all of it.
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [], FaultLabels())
    calls: list[float] = []

    def sample() -> str | None:
        calls.append(clock.now)
        clock.set(clock.now + 0.060)
        return None

    proc = spawn_child("import time; time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, None, 1.050, sample=sample).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: the already-consumed deadline leaves a zero selector wait and then fires.
    assert script.timeouts[0] == 0.0, script.timeouts
    assert result.timed_out and result.first_cause == "collection_deadline", result
    print("PASS callback cost past the deadline leaves a zero selector wait")


def overinterval_callback_cost_adds_no_extra_wait() -> None:
    # Given: a 150 ms callback cost at or above the 100 ms sample interval.
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [], FaultLabels())
    calls: list[float] = []

    def sample() -> str | None:
        calls.append(clock.now)
        clock.set(clock.now + 0.150)
        return "resource_abort fixture stop" if len(calls) > 1 else None

    proc = spawn_child("import time; time.sleep(30)", pipes=False)
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, None, clock.now + 10.0, sample=sample).run()
    finally:
        proc.kill()
        proc.wait(timeout=5)
    # Then: an over-interval cost adds zero wait and the next sample runs at once.
    assert script.timeouts[0] == 0.0, script.timeouts
    assert abs(calls[1] - 1.150) <= 1e-9, calls
    assert result.samples == 2 and "resource_abort" in (result.first_cause or ""), result
    print(f"PASS over-interval callback cost added zero wait; next sample at {calls[1]:.3f}")


def costly_sample_then_fd_fault_still_closes_every_owned_resource() -> None:
    # Given: a real child on real pipes whose selector wait faults after a 60 ms callback cost.
    clock = FakeClock(1.000)
    script = ScriptedSelector(clock, [], FaultLabels(select="injected select EIO (scripted)"))
    calls: list[float] = []

    def sample() -> str | None:
        calls.append(clock.now)
        clock.set(clock.now + 0.060)
        return None

    proc = spawn_child("import time; time.sleep(30)")
    pipes = {proc.stdin.fileno(), proc.stdout.fileno(), proc.stderr.fileno()}
    try:
        with patch("time.monotonic", clock), patch("selectors.DefaultSelector", lambda: script):
            result = Collector(proc, b"x", clock.now + 5.0, sample=sample).run()
        # Then: the FD fault rejects the result and closure is still complete.
        assert calls and result.invalidated
        assert result.first_cause and "selector_select_fault" in result.first_cause
        assert "injected" in result.first_cause
        assert not pipes & set(open_fds()), f"pipe fds held: {pipes & set(open_fds())}"
        assert script.closed
        print("PASS costly callback then FD fault (injected) still closes selector and every pipe")
    finally:
        proc.kill()
        proc.wait(timeout=5)


def main() -> int:
    tests = (
        sample_cost_is_deducted_from_next_sample_wait,
        callback_cost_past_deadline_yields_zero_wait,
        overinterval_callback_cost_adds_no_extra_wait,
        costly_sample_then_fd_fault_still_closes_every_owned_resource,
    )
    failed: list[str] = []
    for test in tests:
        try:
            test()
        except Exception as error:
            failed.append(test.__name__)
            print(f"FAIL {test.__name__}: {type(error).__name__}: {error}")
    print(f"{len(tests) - len(failed)}/{len(tests)} callback-timing wait-remainder tests passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
