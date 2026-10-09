# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/sizing_admission.py
"""Sizing-only 3 GiB admission: exact thresholds, mode selection, distinct mirrors.

The sizing admission floor (3221225472) is its own constant; the ceiling/
prerequisite 4 GiB floor (4294967296) keeps its default everywhere. The
supervisor selects the floor by its existing mode. Admission gates only:
no reservation, no fit claim, no restoration guarantee. Setting authority
stays a local fake; the bundle proof runs real local inotify watch setup
against private fixture roots only, never a target or a kernel write.
"""
from __future__ import annotations

import io
import subprocess
import sys
import tempfile
import time
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from target_safety import bootstrap, report, resource, stage1
from target_safety.report import CEILING_MODE, SIZING_MODE, accepted_sizing_output
from target_safety.resource import ResourceError
from target_safety.supervisor import Supervisor
from ceiling_fixtures import guard_argv
from ceiling_setting_fixtures import fake_setting
from sizing_bundle import run_window, substitute_policy
from sizing_outcomes import complete, ready

GIB = 1024 ** 3
OBSERVED = 3645505536
SIZING_FLOOR = 3221225472
CEILING_FLOOR = 4294967296


def sizing_child_argv(root: Path) -> tuple[str, ...]:
    script = root / "sizing-child.py"
    script.write_text(f"print({chr(10).join((ready(), complete()))!r})\n")
    return (sys.executable, "-B", str(script))


def admission_matrix_exact_thresholds() -> None:
    # Given: the exact sizing and ceiling floors and the observed prior reading.
    assert resource.SIZING_ADMISSION_MEM_AVAILABLE_BYTES == SIZING_FLOOR == 3 * GIB
    assert resource.ADMISSION_MEM_AVAILABLE_BYTES == CEILING_FLOOR == 4 * GIB
    # When/Then: exact 3 GiB sizing passes and one parser kB below fails typed.
    resource.admit(SIZING_FLOOR, resource.SIZING_ADMISSION_MEM_AVAILABLE_BYTES)
    for rejected, threshold in ((SIZING_FLOOR - 1024, resource.SIZING_ADMISSION_MEM_AVAILABLE_BYTES),
                                (OBSERVED, CEILING_FLOOR),
                                (CEILING_FLOOR - 1024, CEILING_FLOOR)):
        try:
            resource.admit(rejected, threshold)
        except ResourceError as error:
            assert error.operation == "admission_mem_available", error
            assert error.detail == f"{rejected} < {threshold}", error
        else:
            raise AssertionError(f"admission accepted {rejected} < {threshold}")
    # Then: the observed 3645505536 passes sizing-only, exact 4 GiB passes ceiling.
    resource.admit(OBSERVED, resource.SIZING_ADMISSION_MEM_AVAILABLE_BYTES)
    resource.admit(CEILING_FLOOR)
    print("PASS exact 3GiB sizing / 1KiB-below reject / observed3645505536 sizing-only / exact 4GiB ceiling / 4GiB-minus reject")


def hold_before_raise_threads_explicit_threshold() -> None:
    # Given: the observed reading at the seam and an explicit sizing threshold.
    with patch.object(bootstrap, "read_mem_available", lambda: OBSERVED):
        # When: the raise is held with the sizing threshold. Then: handshake passes.
        event = bootstrap.hold_before_raise(time.monotonic() + 5.0,
                                            resource.SIZING_ADMISSION_MEM_AVAILABLE_BYTES)
        assert "child_limit_ready" in event
        # When: no threshold is threaded. Then: the 4 GiB default fails closed.
        try:
            bootstrap.hold_before_raise(time.monotonic() + 5.0)
        except ResourceError as error:
            assert error.operation == "admission_mem_available", error
            assert error.detail == f"{OBSERVED} < {CEILING_FLOOR}", error
        else:
            raise AssertionError("default threshold admitted the observed reading")
    print("PASS hold_before_raise threads the explicit threshold; default stays 4GiB")


def policy_mirrors_distinguish_sizing_from_ceiling() -> None:
    sizing_policy, ceiling_policy = resource.SIZING_RESOURCE_POLICY, resource.RESOURCE_POLICY
    assert ceiling_policy["admission_mem_available_bytes"] == CEILING_FLOOR
    assert sizing_policy["admission_mem_available_bytes"] == SIZING_FLOOR
    assert sizing_policy["ceiling_admission_mem_available_bytes"] == CEILING_FLOOR
    assert sizing_policy["admission_scope"] == "topology-sizing-only"
    for key in ("abort_mem_available_below_bytes", "abort_guard_rss_above_bytes",
                "abort_supervisor_rss_above_bytes", "child_only_rlimit_as_bytes",
                "supervisor_sample_interval_max_seconds",
                "guard_resource_check_max_topology_entries", "acknowledgement"):
        assert sizing_policy[key] == ceiling_policy[key], key
    assert report.admission_for_mode(SIZING_MODE) == sizing_policy["admission_mem_available_bytes"]
    assert report.admission_for_mode(CEILING_MODE) == ceiling_policy["admission_mem_available_bytes"]
    print("PASS sizing/ceiling policy mirrors distinguish 3GiB from 4GiB and share all safeguards")


def sizing_mode_admits_observed_reading_and_restores() -> None:
    # Given: fake local authority and the exact reading that failed the old attempt.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        with patch.object(bootstrap, "read_mem_available", lambda: OBSERVED), \
                redirect_stdout(io.StringIO()):
            outcome = Supervisor(setting, sizing_child_argv(root), mode=SIZING_MODE).run()
        assert outcome.accepted and outcome.guard_valid and outcome.restored_original, outcome
        assert outcome.temp_observed == 1048576 and outcome.restore_observed == 65536
        assert state.read_text() == "65536"
    print("PASS sizing mode admits observed3645505536 and restores the exact pair")


def sizing_mode_admits_exact_three_gib() -> None:
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        with patch.object(bootstrap, "read_mem_available", lambda: SIZING_FLOOR), \
                redirect_stdout(io.StringIO()):
            outcome = Supervisor(setting, sizing_child_argv(root), mode=SIZING_MODE).run()
        assert outcome.accepted and outcome.restored_original, outcome
        assert state.read_text() == "65536"
    print("PASS sizing mode admits the exact 3GiB floor")


def sizing_below_floor_rejects_before_write() -> None:
    # Given: one parser kB (1024 B) below the sizing floor under the sizing mode.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        with patch.object(bootstrap, "read_mem_available", lambda: SIZING_FLOOR - 1024), \
                redirect_stdout(io.StringIO()):
            outcome = Supervisor(setting, guard_argv(root, "success"), mode=SIZING_MODE).run()
        assert not outcome.accepted and outcome.guard_exit is None, outcome
        assert outcome.raise_class == "not_raised" and outcome.temp_observed is None
        assert state.read_text() == "65536"
        assert any("admission_mem_available" in e and f"{SIZING_FLOOR - 1024} < {SIZING_FLOOR}" in e
                   for e in outcome.errors), outcome.errors
    print("PASS sizing 1KiB-below rejects BEFORE any write")


def ceiling_mode_keeps_four_gib_unchanged() -> None:
    # Given: the observed reading under the default ceiling mode.
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        with patch.object(bootstrap, "read_mem_available", lambda: OBSERVED), \
                redirect_stdout(io.StringIO()):
            outcome = Supervisor(setting, guard_argv(root, "success")).run()
        assert not outcome.accepted and outcome.raise_class == "not_raised", outcome
        assert state.read_text() == "65536"
        assert any(f"admission_mem_available: {OBSERVED} < {CEILING_FLOOR}" in e
                   for e in outcome.errors), outcome.errors
    print("PASS ceiling mode rejects observed3645505536 at the unchanged 4GiB floor")


def ceiling_exact_four_gib_admits_and_restores() -> None:
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        with patch.object(bootstrap, "read_mem_available", lambda: CEILING_FLOOR), \
                redirect_stdout(io.StringIO()):
            outcome = Supervisor(setting, guard_argv(root, "success")).run()
        assert outcome.accepted and outcome.restored_original, outcome
        assert outcome.temp_observed == 1048576 and state.read_text() == "65536"
    print("PASS ceiling mode admits exact 4GiB and restores")


def ceiling_minus_one_kib_rejects_before_write() -> None:
    with tempfile.TemporaryDirectory(dir="/tmp/opencode") as directory:
        root = Path(directory)
        setting, state = fake_setting(root)
        with patch.object(bootstrap, "read_mem_available", lambda: CEILING_FLOOR - 1024), \
                redirect_stdout(io.StringIO()):
            outcome = Supervisor(setting, guard_argv(root, "success")).run()
        assert not outcome.accepted and outcome.raise_class == "not_raised", outcome
        assert state.read_text() == "65536"
    print("PASS ceiling 4GiB-minus rejects BEFORE any write")


def real_bundle_selects_three_gib_and_keeps_ceiling() -> None:
    # Given: the real current sizing bundle, fake setting, private fixture roots.
    root = Path(tempfile.mkdtemp(prefix="sizing3gib-bundle-", dir="/tmp/opencode"))
    result = run_window(root, mem_available=OBSERVED)
    assert result.returncode == 0, result.stderr or result.stdout
    stdout = result.stdout.decode()
    with substitute_policy(root / "repository", root / "external"):
        assert accepted_sizing_output(stdout), stdout
    # Given: the real current ceiling bundle at the same reading. Then: 4GiB rejects.
    ceiling_root = Path(tempfile.mkdtemp(prefix="sizing3gib-ceiling-", dir="/tmp/opencode"))
    setting, state = fake_setting(ceiling_root)
    preamble = stage1.supervisor_bundle().decode()
    preamble = preamble[: preamble.index("import target_safety.supervisor as _S")]
    injection = (
        "import target_safety.supervisor as _S\n"
        "import target_safety.bootstrap as _B\n"
        f"_B.read_mem_available = lambda: {OBSERVED}\n"
        "import target_safety.setting as _st\n"
        f"fake = _st.SudoSetting(sudo={setting.sudo!r}, sysctl={setting.sysctl!r}, timeout_seconds=5.0)\n"
        f"guard = {b'print(1)'!r}\n"
        "raise SystemExit(_S.main(setting=fake, guard_stdin=guard))\n"
    )
    ceiling_run = subprocess.run((sys.executable, "-B", "-"), input=(preamble + injection).encode(),
                                 capture_output=True, timeout=60, check=False)
    (ceiling_root / "stdout.txt").write_bytes(ceiling_run.stdout)
    assert ceiling_run.returncode == 70, ceiling_run.stderr or ceiling_run.stdout
    ceiling_stdout = ceiling_run.stdout.decode()
    assert f"admission_mem_available: {OBSERVED} < {CEILING_FLOOR}" in ceiling_stdout
    assert state.read_text() == "65536"
    print(f"PASS real current bundles: sizing selects 3GiB (fixture={root}); ceiling stays 4GiB (fixture={ceiling_root})")


def main() -> int:
    admission_matrix_exact_thresholds()
    hold_before_raise_threads_explicit_threshold()
    policy_mirrors_distinguish_sizing_from_ceiling()
    sizing_mode_admits_observed_reading_and_restores()
    sizing_mode_admits_exact_three_gib()
    sizing_below_floor_rejects_before_write()
    ceiling_mode_keeps_four_gib_unchanged()
    ceiling_exact_four_gib_admits_and_restores()
    ceiling_minus_one_kib_rejects_before_write()
    real_bundle_selects_three_gib_and_keeps_ceiling()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
