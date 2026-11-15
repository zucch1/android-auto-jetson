#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualification build guard: an unoptimized build may not start a stage.

Deterministic and hermetic: real ``CMakeCache.txt`` fixtures and the collector's
own CLI against the shipped fake receiver; no target, no hardware, no sleeps
beyond the collector's one-second fixture run.

  refused           Debug, an empty CMAKE_BUILD_TYPE, a missing cache and a
                    missing build directory are all refused, and the collector
                    launches nothing at all
  allowed           Release, RelWithDebInfo and MinSizeRelWithDebInfo pass
  override          --allow-unoptimized-build admits a Debug build with a
                    visible warning (debugging only)
  recorded          the parsed CMAKE_BUILD_TYPE and the cache path are what the
                    operator copies into the run report
  wiring            the perf launcher forwards --build-dir/--allow-unoptimized-build
                    to the on-target collector and reports BLOCKED unoptimized_build
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "perf"
GUARD = TOOL / "aa_build_guard.py"
COLLECTOR = TOOL / "aa_projection_collect.py"
SHELL = TOOL / "aa-projection-perf.sh"
FAKE_RECEIVER = ROOT / "tests" / "perf" / "fixtures" / "wired_perf" / "fake_receiver.py"
REFUSAL = "error: unoptimized_build:"

sys.path.insert(0, str(TOOL))
import aa_build_guard as guard  # noqa: E402

CACHE = """\
# This is the CMakeCache file.
# For build in directory: /build/host-release
# You can edit this file to change values.

########################
# EXECUTABLE LINKER FLAGS
########################

CMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=-O3 -DNDEBUG

########################
# CMAKE
########################

CMAKE_BUILD_TYPE:{kind}={value}
CMAKE_CXX_FLAGS:STRING=-Wall -Wextra -Wpedantic
CMAKE_CXX_FLAGS_RELEASE:STRING=-O3 -DNDEBUG
CMAKE_CXX_FLAGS_RELWITHDEBINFO:STRING=-O2 -g -DNDEBUG
CMAKE_CXX_FLAGS_DEBUG:STRING=-g
"""


def write_cache(directory: Path, kind: str, value: str) -> Path:
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "CMakeCache.txt").write_text(CACHE.format(kind=kind, value=value),
                                              encoding="utf-8")
    return directory


def release(tmp: Path) -> Path:
    return write_cache(tmp / "build-release", "STRING", "Release")


def debug(tmp: Path) -> Path:
    return write_cache(tmp / "build-debug", "STRING", "Debug")


def marker_receiver(tmp: Path) -> Path:
    """A receiver that leaves a marker file: proof the stage really started."""
    marker = tmp / "receiver-started"
    receiver = tmp / "marker-receiver"
    receiver.write_text(f'''#!/usr/bin/env python3
import json, time
open({str(marker)!r}, "w").close()
print('{{"event":"ready","state":"negotiating"}}', flush=True)
print('{{"event":"active","startup_ms":5}}', flush=True)
time.sleep(5)
''', encoding="utf-8")
    receiver.chmod(0o755)
    return receiver


def collector_cli(receiver: Path, state_dir: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(COLLECTOR), "--receiver", str(receiver),
         "--port", "fixture", "--no-display", "--duration-seconds", "1",
         "--startup-timeout-seconds", "1", "--interval", "0.1",
         "--state-dir", str(state_dir), *extra],
        capture_output=True, text=True, timeout=60, check=False)


def test_debug_build_is_refused() -> None:
    # Given a build tree configured with the Debug preset.
    with tempfile.TemporaryDirectory(prefix="guard-debug-") as base:
        directory = debug(Path(base))
        # When the guard inspects it.
        try:
            guard.require_optimized_build(directory)
        except guard.BuildGuardError as error:
            message = str(error)
        else:
            raise AssertionError("Debug build accepted")
        # Then the refusal names the value, the fix and the debugging override.
        assert "CMAKE_BUILD_TYPE=Debug" in message, message
        assert "host-release" in message and "RelWithDebInfo" in message, message
        assert "--allow-unoptimized-build" in message, message
        assert "jetson-aarch64" in message, message


def test_optimized_build_types_are_accepted() -> None:
    # Given each build type that keeps optimization on.
    with tempfile.TemporaryDirectory(prefix="guard-release-") as base:
        for index, value in enumerate(("Release", "RelWithDebInfo", "MinSizeRelWithDebInfo",
                                       "release", "RELWITHDEBINFO")):
            directory = write_cache(Path(base) / str(index), "STRING", value)
            # When inspected, the guard returns the recorded value unchanged.
            assert guard.require_optimized_build(directory) == value, value
            assert guard.is_optimized(guard.build_type_from_cache(directory)), value


def test_unset_build_type_is_refused() -> None:
    # Given builds that never set a build type, or that CMake left NOCONFIG.
    with tempfile.TemporaryDirectory(prefix="guard-unset-") as base:
        for index, value in enumerate(("", "NOCONFIG")):
            directory = write_cache(Path(base) / str(index), "STRING", value)
            try:
                guard.require_optimized_build(directory)
            except guard.BuildGuardError as error:
                assert "CMAKE_BUILD_TYPE" in str(error), str(error)
            else:
                raise AssertionError(f"unoptimized CMAKE_BUILD_TYPE {value!r} accepted")


def test_missing_build_directory_or_cache_is_refused() -> None:
    # Given no build directory, an absent one and a directory without a cache.
    with tempfile.TemporaryDirectory(prefix="guard-missing-") as base:
        empty = Path(base) / "empty-build"
        empty.mkdir()
        for build_dir in (None, "", "   ", Path(base) / "does-not-exist", empty):
            try:
                guard.require_optimized_build(build_dir)
            except guard.BuildGuardError as error:
                assert "--build-dir" in str(error) and "--allow-unoptimized-build" in str(error)
            else:
                raise AssertionError(f"unverifiable build accepted: {build_dir!r}")
        # An explicit cache path is accepted as well as its directory.
        directory = release(Path(base))
        assert guard.build_type_from_cache(directory / "CMakeCache.txt") == "Release"


def test_recorded_build_type_is_the_report_value() -> None:
    # Given a readable optimized cache.
    with tempfile.TemporaryDirectory(prefix="guard-record-") as base:
        directory = release(Path(base))
        # When the collector records the build for the run report.
        line = guard.describe_build(directory, guard.require_optimized_build(directory))
        # Then the value and its cache path are both named.
        assert line.startswith("build: CMAKE_BUILD_TYPE=Release"), line
        assert str(directory / "CMakeCache.txt") in line, line


def test_overridden_build_is_admitted_with_a_warning() -> None:
    # Given a Debug build and the explicit debugging override.
    with tempfile.TemporaryDirectory(prefix="guard-override-") as base:
        directory = debug(Path(base))
        # When the stage is started with the override.
        build_type = guard.require_optimized_build(directory, allow_unoptimized=True)
        # Then it proceeds, reports the real build type and is never silent.
        assert build_type == "Debug"
        result = subprocess.run([sys.executable, "-B", str(GUARD), "--build-dir", str(directory),
                                 "--allow-unoptimized-build"],
                                capture_output=True, text=True, timeout=30, check=False)
        assert result.returncode == 0, result.stderr
        assert "warning: unoptimized_build:" in result.stderr, result.stderr
        assert "CMAKE_BUILD_TYPE=Debug" in result.stderr, result.stderr


def test_guard_cli_refuses_debug_and_accepts_release() -> None:
    with tempfile.TemporaryDirectory(prefix="guard-cli-") as base:
        refused = subprocess.run([sys.executable, "-B", str(GUARD),
                                 "--build-dir", str(debug(Path(base)))],
                                capture_output=True, text=True, timeout=30, check=False)
        assert refused.returncode == 2, refused.stdout
        assert refused.stderr.startswith(REFUSAL), refused.stderr
        assert refused.stdout == "", refused.stdout
        accepted = subprocess.run([sys.executable, "-B", str(GUARD),
                                  "--build-dir", str(release(Path(base)))],
                                 capture_output=True, text=True, timeout=30, check=False)
        assert accepted.returncode == 0, accepted.stderr
        assert "CMAKE_BUILD_TYPE=Release" in accepted.stderr, accepted.stderr


def test_collector_refuses_to_launch_anything_from_a_debug_build() -> None:
    # Given a Debug build directory and a receiver that records being started.
    with tempfile.TemporaryDirectory(prefix="guard-collector-") as base:
        root = Path(base)
        receiver = marker_receiver(root)
        marker = root / "receiver-started"
        # When a qualification stage is requested against that build.
        refused = collector_cli(receiver, root / "state-debug",
                               "--build-dir", str(debug(root)))
        # Then the stage is refused before either process starts.
        assert refused.returncode == 2, refused.stdout + refused.stderr
        assert refused.stderr.startswith(REFUSAL), refused.stderr
        assert not marker.exists(), "the receiver was launched from a Debug build"
        assert refused.stdout == "", refused.stdout
        # And an unverifiable build directory refuses the same way.
        absent = collector_cli(receiver, root / "state-absent",
                               "--build-dir", str(root / "no-such-build"))
        assert absent.returncode == 2, absent.stderr
        assert absent.stderr.startswith(REFUSAL), absent.stderr
        assert not marker.exists(), "the receiver was launched without a verified build"


def test_collector_starts_a_stage_from_an_optimized_build() -> None:
    # Given the Release build directory that produced the binaries.
    with tempfile.TemporaryDirectory(prefix="guard-live-") as base:
        root = Path(base)
        # When the real collector CLI runs against the shipped fake receiver.
        started = collector_cli(FAKE_RECEIVER, root / "state-release",
                                "--build-dir", str(release(root)))
        # Then the stage runs and the capture states the recorded build type.
        assert started.returncode == 0, started.stderr
        assert "build: CMAKE_BUILD_TYPE=Release" in started.stderr, started.stderr
        capture = json.loads(started.stdout)
        assert capture["schema"] == "aa-projection-perf-capture/2"
        assert capture["samples"], capture["samples"]


def test_collector_override_admits_a_debug_build_for_debugging() -> None:
    # Given a Debug build and the explicit override.
    with tempfile.TemporaryDirectory(prefix="guard-override-live-") as base:
        root = Path(base)
        # When the collector is started with it.
        started = collector_cli(FAKE_RECEIVER, root / "state",
                                "--build-dir", str(debug(root)), "--allow-unoptimized-build")
        # Then the run proceeds, and the override is recorded, not hidden.
        assert started.returncode == 0, started.stderr
        assert "warning: unoptimized_build:" in started.stderr, started.stderr
        assert "build: CMAKE_BUILD_TYPE=Debug" in started.stderr, started.stderr
        assert json.loads(started.stdout)["schema"] == "aa-projection-perf-capture/2"


def test_launcher_forwards_the_guard_and_reports_its_own_blocked_reason() -> None:
    # Given the perf launcher that starts the stage over ssh.
    source = SHELL.read_text(encoding="utf-8")
    live = source.split("# ---- live run ----", 1)
    assert len(live) == 2, "launcher lost its live-run section"
    # Then both switches exist and are forwarded to the on-target collector...
    assert 'build_args+=(--build-dir "$BUILD_DIR")' in live[0], "no --build-dir switch"
    assert "build_args+=(--allow-unoptimized-build)" in live[0], "no override switch"
    assert "${build_args_remote}" in live[1], "guard arguments not passed to the collector"
    assert '"aa_build_guard"' in live[1], "guard module not inlined for the target"
    # ...and a refusal becomes its own BLOCKED reason, not a generic failure.
    assert "grep -q '^error: unoptimized_build:'" in live[1], "refusal not detected"
    assert 'blocked_result "unoptimized_build"' in live[1], "no unoptimized_build result"
    # The offline replay path stays a local replay: no guard, no stage, no ssh.
    offline = source.split("# ---- capture mode", 1)[1].split("# ---- live run ----", 1)[0]
    assert "build_args_remote" not in offline and "--build-dir" not in offline


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} qualification build guard tests")
