# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/envelope.py
"""Production envelope constants and actual stdin bundle exposure; no substitution."""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import coverage, deadline, inventory as inventory_module, kernel, stage1
from target_safety import probes
from target_safety.setting import CEILING_ORIGINAL, CEILING_TEMPORARY


def production_constants() -> None:
    """Given the fixed Oracle envelope, When read, Then match exactly and bound each other."""
    assert coverage.PROTECTED_ENTRY_LIMIT == 1000000
    assert kernel.MAX_WATCHES == 1000128
    assert kernel.WINDOW_SECONDS == 900
    assert kernel.WINDOW_TIMEOUT_MESSAGE == f"{kernel.WINDOW_SECONDS} seconds"
    assert inventory_module.INVENTORY_BYTE_LIMIT == 8589934592
    assert stage1.SSH_TIMEOUT_SECONDS == 960
    assert stage1.SSH_KILL_GRACE_SECONDS == 5
    assert stage1.LOCAL_RUN_TIMEOUT_SECONDS == 980
    assert kernel.WINDOW_SECONDS < stage1.SSH_TIMEOUT_SECONDS < stage1.LOCAL_RUN_TIMEOUT_SECONDS
    assert stage1.SSH_TIMEOUT_SECONDS + stage1.SSH_KILL_GRACE_SECONDS <= stage1.LOCAL_RUN_TIMEOUT_SECONDS
    assert f"{stage1.SSH_TIMEOUT_SECONDS}s" in stage1.SSH
    assert f"--kill-after={stage1.SSH_KILL_GRACE_SECONDS}s" in stage1.SSH
    assert stage1.EFFECTIVE_CAPS == {
        "protected_entries": coverage.PROTECTED_ENTRY_LIMIT,
        "registered_watches": kernel.MAX_WATCHES,
        "single_monotonic_window_seconds": kernel.WINDOW_SECONDS,
        "bytes_per_protected_inventory": inventory_module.INVENTORY_BYTE_LIMIT,
        "ssh_timeout_seconds": stage1.SSH_TIMEOUT_SECONDS,
        "ssh_kill_grace_seconds": stage1.SSH_KILL_GRACE_SECONDS,
        "local_subprocess_timeout_seconds": stage1.LOCAL_RUN_TIMEOUT_SECONDS,
        "startup_bound_seconds": deadline.STARTUP_BOUND_SECONDS,
        "supervisor_budget_seconds": deadline.SUPERVISOR_BUDGET_SECONDS,
        "cleanup_headroom_seconds": deadline.CLEANUP_HEADROOM_SECONDS,
        "termination_reap_bound_seconds": deadline.TERMINATION_REAP_BOUND_SECONDS,
        "egress_bound_seconds": deadline.EGRESS_BOUND_SECONDS,
        "scheduling_slack_seconds": deadline.SCHEDULING_SLACK_SECONDS,
        "diagnostic_allowance_seconds": deadline.DIAGNOSTIC_ALLOWANCE_SECONDS,
    }
    print("PASS envelope production constants and 900<960<980 bounds (no substitution)")


def deadline_envelope_invariant() -> None:
    """Given the authoritative allocation, When composed, Then fill GNU 960 exactly."""
    from target_safety.supervisor import (CLEANUP_HEADROOM_SECONDS, SUPERVISOR_BUDGET_SECONDS)
    # Given: the five Oracle envelope terms and the single-source owner module.
    assert (deadline.STARTUP_BOUND_SECONDS + deadline.COLLECTION_BOUND_SECONDS
            + deadline.TERMINATION_REAP_BOUND_SECONDS + deadline.EGRESS_BOUND_SECONDS
            + deadline.SCHEDULING_SLACK_SECONDS) == 960.0
    assert deadline.GNU_TIMEOUT_SECONDS == stage1.SSH_TIMEOUT_SECONDS == 960
    # When: the supervisor-side decomposition is derived. Then: it is exact.
    assert deadline.COLLECTION_BOUND_SECONDS == kernel.WINDOW_SECONDS == 900.0
    assert SUPERVISOR_BUDGET_SECONDS == deadline.SUPERVISOR_BUDGET_SECONDS == 940.0
    assert SUPERVISOR_BUDGET_SECONDS == (deadline.COLLECTION_BOUND_SECONDS
                                         + deadline.TERMINATION_REAP_BOUND_SECONDS
                                         + deadline.EGRESS_BOUND_SECONDS
                                         + deadline.SCHEDULING_SLACK_SECONDS)
    assert CLEANUP_HEADROOM_SECONDS == deadline.CLEANUP_HEADROOM_SECONDS == 40.0
    assert CLEANUP_HEADROOM_SECONDS == (deadline.TERMINATION_REAP_BOUND_SECONDS
                                        + deadline.EGRESS_BOUND_SECONDS
                                        + deadline.SCHEDULING_SLACK_SECONDS)
    assert deadline.STARTUP_BOUND_SECONDS + SUPERVISOR_BUDGET_SECONDS == deadline.GNU_TIMEOUT_SECONDS
    # Then: the aggregate diagnostic allowance is explicit, small against the
    # phases it is carved from, and never wider than the egress bound (the
    # terminal keeps the larger window; heartbeat/sync diagnostics do not).
    assert deadline.DIAGNOSTIC_ALLOWANCE_SECONDS == 1.0
    assert 0.0 < deadline.DIAGNOSTIC_ALLOWANCE_SECONDS <= deadline.EGRESS_BOUND_SECONDS
    assert deadline.DIAGNOSTIC_ALLOWANCE_SECONDS < deadline.TERMINATION_REAP_BOUND_SECONDS
    assert stage1.EFFECTIVE_CAPS["diagnostic_allowance_seconds"] == deadline.DIAGNOSTIC_ALLOWANCE_SECONDS
    # Then: the phase layout inside the budget matches the cleanup formulas
    # (terminate end budget-25, restore end budget-10, egress end budget-5).
    assert SUPERVISOR_BUDGET_SECONDS - 25.0 == deadline.COLLECTION_BOUND_SECONDS + 15.0
    assert SUPERVISOR_BUDGET_SECONDS - 10.0 == deadline.COLLECTION_BOUND_SECONDS + 30.0
    assert SUPERVISOR_BUDGET_SECONDS - deadline.SCHEDULING_SLACK_SECONDS == (
        deadline.COLLECTION_BOUND_SECONDS + deadline.TERMINATION_REAP_BOUND_SECONDS
        + deadline.EGRESS_BOUND_SECONDS)
    print("PASS deadline envelope 20+900+30+5+5=960 and 940=900+30+5+5 decomposition")


def exact_root_ancestors_bound_capacity() -> None:
    # Given: the production roots, with only filesystem observations substituted.
    directory = Path('/tmp/opencode').lstat()
    with kernel.Watch.session() as watch, patch.object(Path, 'lstat', return_value=directory), \
            patch.object(coverage, 'identity', return_value=(0,)), \
            patch.object(kernel.Watch, 'add', autospec=True) as add:
        # When: the real ancestor traversal registers both exact root paths.
        for root in (probes.REPOSITORY, probes.EXTERNAL):
            coverage.ancestors(watch, root)
    registered = [(call.args[1], call.args[2]) for call in add.call_args_list]
    # Then: ten filtered calls need at most seven distinct ancestor descriptors.
    assert registered == [
        (Path('/'), 'home'), (Path('/home'), 'zucchi'),
        (Path('/home/zucchi'), 'Desktop'),
        (Path('/home/zucchi/Desktop'), 'infotainment-plan'),
        (Path('/'), 'home'), (Path('/home'), 'zucchi'),
        (Path('/home/zucchi'), '.omo'), (Path('/home/zucchi/.omo'), 'codegraph'),
        (Path('/home/zucchi/.omo/codegraph'), 'projects'),
        (Path('/home/zucchi/.omo/codegraph/projects'), 'infotainment-plan-68328f6064505b10'),
    ]
    ancestors = len({path for path, _ in registered})
    assert len(registered) == 10 and ancestors == 7
    assert coverage.PROTECTED_ENTRY_LIMIT + ancestors == 1000007 <= kernel.MAX_WATCHES
    assert kernel.MAX_WATCHES - (coverage.PROTECTED_ENTRY_LIMIT + ancestors) == 121
    assert CEILING_ORIGINAL == 65536
    assert kernel.MAX_WATCHES < CEILING_TEMPORARY == 1048576
    assert CEILING_TEMPORARY - kernel.MAX_WATCHES == 48448
    print('PASS exact-root ancestor derivation and E+7/watch/kernel coherence; arithmetic only, no reservation or target fit')


def stdin_bundle_exposes_production_caps() -> None:
    """Given the actual stdin bundle, When loaded, Then expose the production caps."""
    expected = (f"bundle-caps {coverage.PROTECTED_ENTRY_LIMIT} {kernel.MAX_WATCHES} "
                f"{kernel.WINDOW_SECONDS} {inventory_module.INVENTORY_BYTE_LIMIT}")
    entry = (b"from target_safety.coverage import PROTECTED_ENTRY_LIMIT\n"
             b"from target_safety.inventory import INVENTORY_BYTE_LIMIT\n"
             b"from target_safety.kernel import MAX_WATCHES, WINDOW_SECONDS\n"
             b"print('bundle-caps', PROTECTED_ENTRY_LIMIT, MAX_WATCHES,"
             b" WINDOW_SECONDS, INVENTORY_BYTE_LIMIT)\n")
    payload = stage1.bundle().rsplit(b"raise SystemExit", 1)[0] + entry
    result = subprocess.run([sys.executable, "-B", "-"], input=payload,
                            capture_output=True, check=False, timeout=20)
    assert result.returncode == 0, result.stderr
    assert expected.encode() in result.stdout, result.stdout
    print("PASS envelope actual stdin bundle exposes production caps")


def resource_and_mode_policy() -> None:
    """Given the resource/sizing policy, When read, Then mirror exactly and stay exact-valued."""
    from target_safety import collector, resource, routes, sizing
    from target_safety import report
    from target_safety.setting import CEILING_SETTING, classify, reconcile_raise, reconcile_restore
    policy = resource.RESOURCE_POLICY
    assert set(policy) == {"admission_mem_available_bytes", "abort_mem_available_below_bytes",
                           "abort_guard_rss_above_bytes", "abort_supervisor_rss_above_bytes",
                           "child_only_rlimit_as_bytes", "supervisor_sample_interval_max_seconds",
                           "guard_resource_check_max_topology_entries", "acknowledgement"}
    assert policy["admission_mem_available_bytes"] == 4294967296
    assert policy["abort_mem_available_below_bytes"] == 2147483648
    assert policy["abort_guard_rss_above_bytes"] == 1073741824
    assert policy["abort_supervisor_rss_above_bytes"] == 134217728
    assert policy["child_only_rlimit_as_bytes"] == 2147483648
    assert policy["supervisor_sample_interval_max_seconds"] == 0.1
    assert policy["guard_resource_check_max_topology_entries"] == 64
    assert policy["acknowledgement"]
    assert resource.ADMISSION_MEM_AVAILABLE_BYTES == 4 * 2 ** 30
    assert resource.SIZING_ADMISSION_MEM_AVAILABLE_BYTES == 3 * 2 ** 30
    sizing_policy = resource.SIZING_RESOURCE_POLICY
    assert sizing_policy["admission_mem_available_bytes"] == 3 * 2 ** 30
    assert sizing_policy["ceiling_admission_mem_available_bytes"] == 4 * 2 ** 30
    assert sizing_policy["admission_scope"] == "topology-sizing-only"
    assert report.admission_for_mode(report.SIZING_MODE) == sizing_policy["admission_mem_available_bytes"]
    assert report.admission_for_mode(report.CEILING_MODE) == policy["admission_mem_available_bytes"]
    assert [kind for name, kind in routes._table() if name == "task-5-topology-sizing-3gib.json"] == ["sizing"]
    assert resource.ABORT_MEM_AVAILABLE_BELOW_BYTES == 2 * 2 ** 30
    assert collector.COMBINED_OUTPUT_LIMIT_BYTES == 16777216
    assert collector.RECORD_LIMIT_BYTES == 65536
    assert collector.SAMPLE_INTERVAL_SECONDS <= 0.1
    assert sizing.MODE == report.SIZING_MODE == "topology-sizing"
    assert sizing.COMPLETION_EVENT == "topology_sizing_complete"
    assert report.SIZING_OUTCOME_EVENT != report.CEILING_OUTCOME_EVENT
    assert routes.SIZING == "task-5-topology-sizing.json"
    assert routes.SIZING_RSS_LIFECYCLE == "task-5-topology-sizing-rss-lifecycle.json"
    assert [kind for name, kind in routes._table() if name == routes.SIZING_RSS_LIFECYCLE] == ["sizing"]
    assert report.admission_for_mode(report.SIZING_MODE) == 3221225472
    assert classify(CEILING_TEMPORARY) == "temporary" and classify(65535) == "conflict"
    assert reconcile_raise(CEILING_TEMPORARY).action == "proceed"
    assert reconcile_restore(CEILING_TEMPORARY).write_value == CEILING_ORIGINAL
    assert reconcile_raise(CEILING_TEMPORARY - 1).action == "conflict_no_overwrite"
    assert CEILING_SETTING == "fs.inotify.max_user_watches"
    print("PASS resource/sizing policy mirror, exact write pair and mode vocabulary")


def main() -> int:
    exact_root_ancestors_bound_capacity()
    production_constants()
    deadline_envelope_invariant()
    resource_and_mode_policy()
    stdin_bundle_exposes_production_caps()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
