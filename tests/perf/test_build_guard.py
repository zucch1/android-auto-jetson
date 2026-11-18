#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualification build guard: an unoptimized build may not start a stage.

Deterministic and hermetic: real ``CMakeCache.txt`` fixtures and the collector's
own CLI against the shipped fake receiver; no target, no hardware, no sleeps
beyond the collector's one-second fixture run.

  refused           Debug, an empty CMAKE_BUILD_TYPE, a missing cache and a
                    missing build directory are all refused, and the collector
                    launches nothing at all
  allowed           Release, RelWithDebInfo and MinSizeRel pass, case-insensitive;
                    MinSizeRelWithDebInfo is not a CMake build type and is refused
  override          --allow-unoptimized-build admits a Debug build with a
                    visible warning (debugging only)
  recorded          the parsed CMAKE_BUILD_TYPE and the cache path are what the
                    operator copies into the run report
  binaries          a launched binary belongs to the build directory: in-tree,
                    a byte-identical deployed copy, or derived from the tree for
                    a cross build; anything else is refused, symlinks resolved
  qualification     an override run records build type, build directory and the
                    override in the capture and the result, and is never a PASS:
                    it is NOT_QUALIFYING while its measured values stay reported
  display_env       the launcher honours --display-env (and the caller's
                    DISPLAY/XAUTHORITY), with :1 only as the fallback
  wiring            the perf launcher forwards --build-dir/--allow-unoptimized-build
                    to the on-target collector and reports BLOCKED unoptimized_build
"""
from __future__ import annotations

import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools" / "perf"
GUARD = TOOL / "aa_build_guard.py"
COLLECTOR = TOOL / "aa_projection_collect.py"
SHELL = TOOL / "aa-projection-perf.sh"
REPORTER = TOOL / "aa_perf_report.py"
FAKE_RECEIVER = ROOT / "tests" / "perf" / "fixtures" / "wired_perf" / "fake_receiver.py"
NOMINAL = ROOT / "tests" / "perf" / "fixtures" / "wired_perf" / "nominal_capture.json"
OVERLOAD = ROOT / "tests" / "perf" / "fixtures" / "wired_perf" / "overload_capture.json"
REFUSAL = "error: unoptimized_build:"
MISMATCH = "error: build_dir_mismatch:"
NOT_QUALIFYING = "NOT_QUALIFYING: unoptimized_build"
NOW = "2026-10-13T00:00:00Z"

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


def marker_receiver(tmp: Path, directory: Path | None = None, name: str = "marker-receiver") -> Path:
    """A receiver that leaves a marker file: proof the stage really started."""
    marker = tmp / "receiver-started"
    target = (directory or tmp)
    target.mkdir(parents=True, exist_ok=True)
    receiver = target / name
    receiver.write_text(f'''#!/usr/bin/env python3
import json, time
open({str(marker)!r}, "w").close()
print('{{"event":"ready","state":"negotiating"}}', flush=True)
print('{{"event":"active","startup_ms":5}}', flush=True)
time.sleep(5)
''', encoding="utf-8")
    receiver.chmod(0o755)
    return receiver


def install_receiver(build_dir: Path, name: str = "aa-wired-receiver") -> Path:
    """A copy of the shipped fake receiver inside a build directory."""
    build_dir.mkdir(parents=True, exist_ok=True)
    target = build_dir / name
    shutil.copy2(FAKE_RECEIVER, target)
    target.chmod(0o755)
    return target


def collector_cli(receiver: Path, state_dir: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(COLLECTOR), "--receiver", str(receiver),
         "--port", "fixture", "--no-display", "--duration-seconds", "1",
         "--startup-timeout-seconds", "1", "--interval", "0.1",
         "--state-dir", str(state_dir), *extra],
        capture_output=True, text=True, timeout=60, check=False)


def report_capture(capture: Path, out: Path, *extra: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, "-B", str(REPORTER), "--capture", str(capture), "--out", str(out),
         "--now", NOW, *extra],
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
        for index, value in enumerate(("Release", "RelWithDebInfo", "MinSizeRel",
                                       "release", "RELWITHDEBINFO", "minsizerel")):
            directory = write_cache(Path(base) / str(index), "STRING", value)
            # When inspected, the guard returns the recorded value unchanged.
            assert guard.require_optimized_build(directory) == value, value
            assert guard.is_optimized(guard.build_type_from_cache(directory)), value


def test_minsizerelwithdebinfo_is_not_a_build_type() -> None:
    # Given the value this guard used to accept, which CMake never produces.
    with tempfile.TemporaryDirectory(prefix="guard-typo-") as base:
        directory = write_cache(Path(base), "STRING", "MinSizeRelWithDebInfo")
        # Then it is refused like any other unoptimized value, not silently kept.
        assert not guard.is_optimized("MinSizeRelWithDebInfo")
        try:
            guard.require_optimized_build(directory)
        except guard.BuildGuardError as error:
            assert "CMAKE_BUILD_TYPE=MinSizeRelWithDebInfo" in str(error), str(error)
        else:
            raise AssertionError("MinSizeRelWithDebInfo accepted as an optimized build")


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
        build = release(root)
        receiver = install_receiver(build)
        # When the real collector CLI runs against the shipped fake receiver.
        started = collector_cli(receiver, root / "state-release", "--build-dir", str(build))
        # Then the stage runs and the capture states the recorded build type.
        assert started.returncode == 0, started.stderr
        assert "build: CMAKE_BUILD_TYPE=Release" in started.stderr, started.stderr
        capture = json.loads(started.stdout)
        assert capture["schema"] == "aa-projection-perf-capture/3"
        assert capture["samples"], capture["samples"]
        # And the capture records which build produced the measured binaries.
        recorded = capture["build"]
        assert recorded["build_type"] == "Release", recorded
        assert recorded["build_dir"] == str(build), recorded
        assert recorded["allow_unoptimized_build"] is False, recorded
        assert recorded["verified_on"] == "target", recorded
        assert recorded["binaries"]["receiver"]["verified"] == "in_build_dir", recorded


def test_collector_override_admits_a_debug_build_for_debugging() -> None:
    # Given a Debug build and the explicit override.
    with tempfile.TemporaryDirectory(prefix="guard-override-live-") as base:
        root = Path(base)
        build = debug(root)
        receiver = install_receiver(build)
        # When the collector is started with it.
        started = collector_cli(receiver, root / "state",
                                "--build-dir", str(build), "--allow-unoptimized-build")
        # Then the run proceeds, and the override is recorded, not hidden.
        assert started.returncode == 0, started.stderr
        assert "warning: unoptimized_build:" in started.stderr, started.stderr
        assert "build: CMAKE_BUILD_TYPE=Debug" in started.stderr, started.stderr
        capture = json.loads(started.stdout)
        assert capture["schema"] == "aa-projection-perf-capture/3"
        assert capture["build"]["build_type"] == "Debug", capture["build"]
        assert capture["build"]["allow_unoptimized_build"] is True, capture["build"]


def test_binary_from_another_build_is_refused() -> None:
    # Given a build directory that produced its own receiver...
    with tempfile.TemporaryDirectory(prefix="guard-foreign-") as base:
        root = Path(base)
        build = release(root)
        install_receiver(build, "aa-wired-receiver")
        # ...and another build's receiver, launched through a symlink that leaves
        # the build tree and through a plain path with the same basename.
        foreign_build = write_cache(root / "build-other", "STRING", "Release")
        foreign = install_receiver(foreign_build, "aa-wired-receiver")
        foreign.write_text(foreign.read_text(encoding="utf-8") + "\n# another build\n",
                           encoding="utf-8")
        escape = build / "escaped"
        escape.symlink_to(foreign)
        for launched in (foreign, escape):
            # When a stage is requested with binaries this build did not produce.
            refused = collector_cli(launched, root / "state", "--build-dir", str(build))
            # Then it is refused as a build-directory mismatch, not a slow build,
            # and nothing is launched.
            assert refused.returncode == 2, refused.stdout + refused.stderr
            assert refused.stderr.startswith(MISMATCH), refused.stderr
            assert str(build) in refused.stderr, refused.stderr
            assert refused.stdout == "", refused.stdout
        # And a byte-identical deployed copy of that build's binary is accepted.
        deployed = root / "deployed"
        deployed.mkdir()
        copy_source = install_receiver(build, "aa-wired-receiver")
        shutil.copy2(copy_source, deployed / "aa-wired-receiver")
        (deployed / "aa-wired-receiver").chmod(0o755)
        accepted = collector_cli(deployed / "aa-wired-receiver", root / "state-copy",
                                 "--build-dir", str(build))
        assert accepted.returncode == 0, accepted.stderr
        verified = json.loads(accepted.stdout)["build"]["binaries"]["receiver"]
        assert verified["verified"] == "identical_copy", verified


def test_cross_build_binaries_are_derived_from_the_local_build_dir() -> None:
    # Given a cross build: the tree is on this host, the binaries run elsewhere.
    with tempfile.TemporaryDirectory(prefix="guard-cross-") as base:
        root = Path(base)
        build = release(root)
        # Then a binary the tree did not produce is refused as a mismatch...
        try:
            guard.derive_binaries_in_build_dir(build, {"receiver": "aa-wired-receiver"})
        except guard.BuildDirMismatchError as error:
            assert "aa-wired-receiver" in str(error) and "no executable" in str(error), str(error)
        else:
            raise AssertionError("a binary absent from the build tree was derived")
        # ...while a produced one resolves to its path in that tree.
        install_receiver(build, "aa-wired-receiver")
        record = guard.derive_binaries_in_build_dir(build, {"receiver": "aa-wired-receiver"})
        assert record["receiver"]["verified"] == "derived_from_build_dir", record
        assert str(build) in record["receiver"]["build_path"], record
        # The same derivation through the guard CLI is a machine-readable record.
        checked = subprocess.run(
            [sys.executable, "-B", str(GUARD), "--build-dir", str(build),
             "--expect", "receiver=aa-wired-receiver", "--json"],
            capture_output=True, text=True, timeout=30, check=False)
        assert checked.returncode == 0, checked.stderr
        assert json.loads(checked.stdout)["build_type"] == "Release", checked.stdout
        # The target records the host-side verification, never a target-verified one.
        host_verified = collector_cli(
            install_receiver(build), root / "state-host",
            "--build-dir", str(build), "--build-dir-verified-locally", "Release")
        assert host_verified.returncode == 0, host_verified.stderr
        recorded = json.loads(host_verified.stdout)["build"]
        assert recorded["verified_on"] == "host", recorded
        assert recorded["build_type"] == "Release", recorded


def test_cross_build_host_verification_still_refuses_debug() -> None:
    # Given a host-verified build type that is not optimized.
    with tempfile.TemporaryDirectory(prefix="guard-cross-debug-") as base:
        root = Path(base)
        receiver = install_receiver(debug(root))
        refused = collector_cli(receiver, root / "state",
                                "--build-dir", "/host/build-debug",
                                "--build-dir-verified-locally", "Debug")
        # Then the target refuses it without the override, and records it with.
        assert refused.returncode == 2, refused.stderr
        assert refused.stderr.startswith(REFUSAL), refused.stderr
        assert "Debug" in refused.stderr, refused.stderr
        overridden = collector_cli(receiver, root / "state",
                                   "--build-dir", "/host/build-debug",
                                   "--build-dir-verified-locally", "Debug",
                                   "--allow-unoptimized-build")
        assert overridden.returncode == 0, overridden.stderr
        recorded = json.loads(overridden.stdout)["build"]
        assert recorded["allow_unoptimized_build"] is True, recorded
        assert recorded["verified_on"] == "host", recorded


def test_override_run_is_recorded_and_never_qualifies() -> None:
    # Given the nominal capture that reports PASS, recorded as a run the guard
    # admitted only through its debugging override.
    with tempfile.TemporaryDirectory(prefix="guard-qual-") as base:
        root = Path(base)
        nominal = json.loads(NOMINAL.read_text(encoding="utf-8"))
        plain_capture = root / "plain.json"
        plain_capture.write_text(json.dumps(nominal), encoding="utf-8")
        plain_out = root / "plain-result.json"
        assert report_capture(plain_capture, plain_out).returncode == 0, "nominal must PASS"
        plain = json.loads(plain_out.read_text(encoding="utf-8"))
        assert plain["verdict"] == "PASS"
        assert plain["build"]["build_type"] is None and plain["build"]["non_qualifying"] is None

        overridden_capture = copy.deepcopy(nominal)
        overridden_capture["build"] = {"build_type": "Debug", "build_dir": "/build/host-dev",
                                       "allow_unoptimized_build": True, "verified_on": "target"}
        override_path = root / "override.json"
        override_path.write_text(json.dumps(overridden_capture), encoding="utf-8")
        override_out = root / "override-result.json"
        # When it is reported.
        result = report_capture(override_path, override_out)
        # Then it is not a qualification PASS: the verdict, the reason and the
        # build record all say so...
        assert result.returncode == 3, result.stdout + result.stderr
        doc = json.loads(override_out.read_text(encoding="utf-8"))
        assert doc["verdict"] == "BLOCKED", doc["verdict"]
        assert doc["blocked_on"] == NOT_QUALIFYING, doc["blocked_on"]
        assert doc["qualification_status"] == "INCOMPLETE", doc["qualification_status"]
        assert doc["build"]["allow_unoptimized_build"] is True, doc["build"]
        assert doc["build"]["build_type"] == "Debug", doc["build"]
        assert doc["build"]["build_dir"] == "/build/host-dev", doc["build"]
        assert doc["build"]["non_qualifying"] == NOT_QUALIFYING, doc["build"]
        assert any(NOT_QUALIFYING in note for note in doc["notes"]), doc["notes"]
        # ...while the measured values are still reported, unchanged.
        assert {name: entry["value"] for name, entry in doc["metrics"].items()} == \
               {name: entry["value"] for name, entry in plain["metrics"].items()}

        # A declared-unoptimized build type is treated the same way even without
        # the override flag, and a measured budget miss still FAILs.
        declared = copy.deepcopy(nominal)
        declared["build"] = {"build_type": "Debug", "build_dir": "/build/host-dev",
                             "allow_unoptimized_build": False, "verified_on": "target"}
        declared_path = root / "declared.json"
        declared_path.write_text(json.dumps(declared), encoding="utf-8")
        declared_out = root / "declared-result.json"
        assert report_capture(declared_path, declared_out).returncode == 3
        assert json.loads(declared_out.read_text(encoding="utf-8"))["blocked_on"] == NOT_QUALIFYING

        failing = json.loads(OVERLOAD.read_text(encoding="utf-8"))
        failing["build"] = {"build_type": "Debug", "build_dir": "/build/host-dev",
                            "allow_unoptimized_build": True, "verified_on": "target"}
        failing_path = root / "failing.json"
        failing_path.write_text(json.dumps(failing), encoding="utf-8")
        failing_out = root / "failing-result.json"
        assert report_capture(failing_path, failing_out).returncode == 1, "a miss still FAILs"
        failing_doc = json.loads(failing_out.read_text(encoding="utf-8"))
        assert failing_doc["verdict"] == "FAIL" and failing_doc["violations"], failing_doc["verdict"]
        assert failing_doc["build"]["non_qualifying"] == NOT_QUALIFYING, failing_doc["build"]

        # And the capture the collector wrote carries the same three values.
        live = collector_cli(install_receiver(debug(root)), root / "state-live",
                             "--build-dir", str(root / "build-debug"),
                             "--allow-unoptimized-build")
        assert live.returncode == 0, live.stderr
        live_capture = json.loads(live.stdout)
        live_path = root / "live.json"
        live_path.write_text(json.dumps(live_capture), encoding="utf-8")
        assert live_capture["build"] == {
            "build_type": "Debug", "build_dir": str(root / "build-debug"),
            "allow_unoptimized_build": True, "verified_on": "target",
            "binaries": {"receiver": {"launched": str(root / "build-debug" / "aa-wired-receiver"),
                                      "build_path": str(root / "build-debug" / "aa-wired-receiver"),
                                      "verified": "in_build_dir"}},
        }, live_capture["build"]
        live_out = root / "live-result.json"
        report_capture(live_path, live_out, "--live")
        assert json.loads(live_out.read_text(encoding="utf-8"))["build"]["non_qualifying"] == \
            NOT_QUALIFYING


def live_capture(root: Path, **build) -> Path:
    """A capture that reports PASS when replayed as a live run."""
    capture = json.loads(NOMINAL.read_text(encoding="utf-8"))
    capture.update(early_exit=False, display_connected=True, receiver_exit_code=0,
                   display_survived=True, terminal_errors=[],
                   receiver_stopped_by_collector=False)
    digest = "sha256:" + "a" * 64
    capture["provenance"] = {
        "source_digest": digest, "build_artifact_digest": digest,
        "configuration_digest": digest,
        "service_definitions": [{"name": "aa-wired.service", "digest": digest}],
        "dependency_versions": {"ffmpeg": "6.1"}, "target_hardware_state": {},
    }
    capture["events"].update(startup_ms=1200, video_units=600, audio_chunks=300)
    if build:
        capture["build"] = {"build_dir": "/opt/aa/build", "verified_on": "target",
                            "allow_unoptimized_build": False, **build}
    path = root / f"remote-{abs(hash(json.dumps(sorted(build.items()))))}.json"
    path.write_text(json.dumps(capture), encoding="utf-8")
    return path


def stub_ssh(root: Path) -> tuple[Path, Path]:
    """A PATH-local ssh stand-in: records the remote command, serves fixtures."""
    stub_dir = root / "bin"
    stub_dir.mkdir()
    log = root / "ssh.log"
    stub = stub_dir / "ssh"
    stub.write_text(f'''#!/usr/bin/env bash
printf '%s\\n' "$*" >> {str(log)!r}
command_line="$*"
case "$command_line" in
    *"bash -s -- "*) eval "${{@: -1}}"; exit $? ;;  # the remote stop, run on this "target"
    *CMakeCache*) [ -n "${{STUB_CACHE:-}}" ] && [ -f "$STUB_CACHE" ] && exit 0 || exit 1 ;;
    *"python3 -"*) [ -n "${{STUB_CAPTURE:-}}" ] || exit 1
                   cat "$STUB_CAPTURE"
                   printf '%s\\n' "${{STUB_BUILD_LINE:-}}" >&2
                   exit 0 ;;
    *sha256sum*aa-dummy-display*) printf '%s  d\\n' "${{STUB_DISPLAY_SHA:-}}"; exit 0 ;;
    *"adb devices -l"*) printf 'List of devices attached\\nTESTPHONE01 device usb:1-2.1 model:FAKE\\n'; exit 0 ;;
    *sha256sum*) printf '%s  r\\n' "${{STUB_RECEIVER_SHA:-}}"; exit 0 ;;
    *"test -x"*) exit 0 ;;
    *xrandr* | *drm*) printf 'connected\\n'; exit 0 ;;
    *pactl* | *pw-cli*) printf '0\\tfake\\tPipeWire\\ts32le 2ch 48000Hz\\tIDLE\\n'; exit 0 ;;
esac
exit 0
''', encoding="utf-8")
    stub.chmod(0o755)
    return stub_dir, log


def drive_smoke_record(root: Path) -> Path:
    """A passing #137 drive smoke for the stub's binary hashes (the soak gate)."""
    path = root / "drive-smoke.json"
    path.write_text(json.dumps({
        "schema": "aa-drive-smoke/2", "verdict": "PASS", "duration_s": 600,
        "video_profiles": None, "phone_serial": "TESTPHONE01",
        "configuration": {"receiver_flags": [], "display_flags": [], "receiver_decoder": "default",
                          "display_decoder": "default", "presentation": "default"},
        # Live freshness uses the real clock (the launcher never forwards --now).
        "finished_utc": datetime.now(timezone.utc).isoformat(),
        "binaries": {"receiver": {"sha256": "a" * 64}, "display": {"sha256": "b" * 64}}}),
        encoding="utf-8")
    return path


def test_launcher_records_the_build_and_never_passes_an_override_run() -> None:
    # Given a target whose ssh is a stand-in that serves a PASSing capture and
    # records the remote command it was handed.
    with tempfile.TemporaryDirectory(prefix="guard-launcher-") as base:
        root = Path(base)
        stub_dir, log = stub_ssh(root)
        smoke = drive_smoke_record(root)

        def launcher(capture: Path, build_line: str, cache: str, *extra: str
                     ) -> subprocess.CompletedProcess:
            env = {"PATH": f"{stub_dir}:/usr/bin:/bin", "AA_PERF_HOST": "fake.local",
                   "AA_DISPLAY": ":0", "STUB_CAPTURE": str(capture),
                   "STUB_BUILD_LINE": build_line, "STUB_CACHE": cache,
                   "STUB_RECEIVER_SHA": "a" * 64, "STUB_DISPLAY_SHA": "b" * 64}
            return subprocess.run(
                ["bash", str(SHELL), "--mode", "wired", "--duration", "1s",
                 "--out", str(root / "result.json"), "--now", NOW,
                 "--drive-smoke", str(smoke), "--phone-serial", "TESTPHONE01",
                 "--media-uri", "file:///sdcard/Music/fixture.opus",
                 "--media-component", "org.example.player/.Play", *extra],
                capture_output=True, text=True, timeout=120, env=env, check=False)

        # When the stage runs from the build directory that produced the binaries.
        target_cache = str(release(root) / "CMakeCache.txt")
        plain = launcher(live_capture(root, build_type="Release"),
                         "build: CMAKE_BUILD_TYPE=Release (from /opt/aa/build/CMakeCache.txt)",
                         target_cache, "--build-dir", str(root / "build-release"))
        # Then the run is a qualification PASS and the collector's build record
        # stayed in the output instead of being dropped on exit 0.
        assert plain.returncode == 0, plain.stdout + plain.stderr
        doc = json.loads((root / "result.json").read_text(encoding="utf-8"))
        assert doc["verdict"] == "PASS" and doc["qualification_status"] == "COMPLETE_LIVE"
        assert doc["build"]["build_type"] == "Release", doc["build"]
        assert doc["build"]["allow_unoptimized_build"] is False, doc["build"]
        assert doc["build"]["non_qualifying"] is None, doc["build"]
        assert "build: CMAKE_BUILD_TYPE=Release" in plain.stderr, plain.stderr

        # When the same stage is started with the qualification override...
        overridden = launcher(live_capture(root, build_type="Debug",
                                           allow_unoptimized_build=True), "",
                              target_cache, "--build-dir", str(root / "build-release"),
                              "--allow-unoptimized-build")
        # Then it is not a PASS: the result names the override and cannot be read
        # as a qualification run, while its measured values stay in the report.
        assert overridden.returncode == 3, overridden.stdout + overridden.stderr
        doc = json.loads((root / "result.json").read_text(encoding="utf-8"))
        assert doc["verdict"] == "BLOCKED", doc["verdict"]
        assert doc["blocked_on"] == NOT_QUALIFYING, doc["blocked_on"]
        assert doc["qualification_status"] == "INCOMPLETE", doc["qualification_status"]
        assert doc["build"]["allow_unoptimized_build"] is True, doc["build"]
        assert doc["build"]["build_type"] == "Debug", doc["build"]
        assert doc["build"]["non_qualifying"] == NOT_QUALIFYING, doc["build"]
        assert doc["metrics"]["av_sync_ms"]["value"] is not None, doc["metrics"]

        # When the build tree is only on this host (cross build), the launcher
        # verifies it locally and the target records the host-side check.
        log.unlink()
        cross_build = release(root / "cross")
        install_receiver(cross_build, "aa-wired-receiver")
        install_receiver(cross_build, "aa-dummy-display")  # the display is measured too
        cross = launcher(live_capture(root, build_type="Release"), "", str(root / "absent"),
                         "--build-dir", str(cross_build))
        assert cross.returncode == 0, cross.stdout + cross.stderr
        assert json.loads((root / "result.json").read_text(encoding="utf-8"))["verdict"] == "PASS"
        recorded = log.read_text(encoding="utf-8")
        assert f"--build-dir-verified-locally 'Release'" in recorded, recorded
        # And a local build tree that did not produce the launched binaries is
        # refused before anything runs on the target.
        log.unlink()
        empty_build = release(root / "cross-empty")
        refused = launcher(live_capture(root, build_type="Release"), "", str(root / "absent"),
                           "--build-dir", str(empty_build))
        assert refused.returncode == 3, refused.stdout + refused.stderr
        assert "BLOCKED: build_dir_mismatch" in refused.stderr, refused.stderr
        assert json.loads((root / "result.json").read_text(encoding="utf-8"))["blocked_on"] == \
            "build_dir_mismatch"
        assert "python3 -" not in log.read_text(encoding="utf-8"), "the stage was launched anyway"


def test_launcher_honours_the_callers_display_environment() -> None:
    # Given a stub ssh that records the remote command it was handed.
    with tempfile.TemporaryDirectory(prefix="guard-display-") as base:
        root = Path(base)
        stub_dir = root / "bin"
        stub_dir.mkdir()
        log = root / "ssh.log"
        stub = stub_dir / "ssh"
        stub.write_text(f'''#!/usr/bin/env bash
printf '%s\\n' "$*" >> {str(log)!r}
exit 255
''', encoding="utf-8")
        stub.chmod(0o755)

        def launcher(*extra: str, env: dict[str, str] | None = None) -> subprocess.CompletedProcess:
            base_env = {"PATH": f"{stub_dir}:/usr/bin:/bin", "AA_PERF_HOST": "fake.local"}
            base_env.update(env or {})
            return subprocess.run(["bash", str(SHELL), "--readiness-only",
                                   "--out", str(root / "readiness.json"), *extra],
                                  capture_output=True, text=True, timeout=60,
                                  env=base_env, check=False)

        # When nothing names a display, :1 is the documented fallback.
        assert launcher().returncode == 3
        assert "DISPLAY=:1" in log.read_text(encoding="utf-8"), log.read_text(encoding="utf-8")
        log.unlink()
        # When the caller's session environment names one, it is honoured...
        assert launcher(env={"DISPLAY": ":0",
                             "XAUTHORITY": "/run/user/1000/gdm/Xauthority"}).returncode == 3
        recorded = log.read_text(encoding="utf-8")
        assert "DISPLAY=:0" in recorded, recorded
        assert "XAUTHORITY=/run/user/1000/gdm/Xauthority" in recorded, recorded
        assert "DISPLAY=:1" not in recorded, recorded
        log.unlink()
        # ...and --display-env overrides both, with or without a cookie.
        assert launcher("--display-env", ":2,/tmp/cookie", env={"DISPLAY": ":0"}).returncode == 3
        recorded = log.read_text(encoding="utf-8")
        assert "DISPLAY=:2" in recorded and "XAUTHORITY=/tmp/cookie" in recorded, recorded
        log.unlink()
        assert launcher("--display-env", ":3").returncode == 3
        assert "DISPLAY=:3" in log.read_text(encoding="utf-8")
        # A value that could inject a remote command is a usage error.
        bad = launcher("--display-env", ":0;id")
        assert bad.returncode == 2, bad.stderr
        assert "usage error" in bad.stderr, bad.stderr


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
    # ...a refusal becomes its own BLOCKED reason, not a generic failure...
    assert "grep -q '^error: unoptimized_build:'" in live[1], "refusal not detected"
    assert 'blocked_result "unoptimized_build"' in live[1], "no unoptimized_build result"
    # ...a binary another build produced has its own reason...
    assert "grep -q '^error: build_dir_mismatch:'" in live[1], "mismatch not detected"
    assert 'blocked_result "build_dir_mismatch"' in live[1], "no build_dir_mismatch result"
    # ...a cross build's local cache is checked before anything is launched...
    assert "--build-dir-verified-locally" in live[1], "host verification not forwarded"
    assert "build-guard.err" in live[1], "local CMakeCache.txt not checked"
    # ...the guard's own stderr lines survive a successful stage...
    assert 'sed -n \'1,40p\' "$BUFFER/collector.err" >&2' in live[1], "build record dropped"
    # ...and the blocked result records the build it was about.
    assert 'blocked_result "$ready_reason" "" "$BUILD_DIR" "$ALLOW_UNOPTIMIZED"' in live[1]
    # The offline replay path stays a local replay: no guard, no stage, no ssh.
    offline = source.split("# ---- capture mode", 1)[1].split("# ---- live run ----", 1)[0]
    assert "build_args_remote" not in offline and "--build-dir" not in offline


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} qualification build guard tests")
