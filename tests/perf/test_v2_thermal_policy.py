#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Approved Nano availability exception; synthetic offline evidence only."""
from __future__ import annotations

import argparse
import copy
import json
import subprocess
import tempfile
from pathlib import Path
from typing import assert_never
from unittest.mock import patch

from test_v2_coverage import result, synthetic
from test_continuity_regressions import Process
import aa_measurement_coverage as coverage
import aa_perf_schema as schema
import aa_projection_collect as collector

MODEL = "NVIDIA Jetson Orin Nano Engineering Reference Developer Kit Super"
PROFILE = "orin-nano-cv-unavailable/1"
POLICY = {"profile": PROFILE, "model": MODEL,
          "approval": "owner-approved-nano-cv-availability-20261018",
          "scope": "continuous-cpu-gpu-soc0-soc1-soc2-tj;cv0-cv1-cv2-availability-exception"}
FAMILIES = ("cpu", "gpu", "cv0", "cv1", "cv2", "soc0", "soc1", "soc2", "tj")
ROOT = Path(__file__).resolve().parents[2]


def nano_capture(seconds: int = 20) -> dict:
    raw = synthetic(seconds)
    raw["thermal_policy"] = dict(POLICY)
    raw["target_hardware_state"] = {"model": MODEL}
    raw["provenance"]["target_hardware_state"] = {"model": MODEL}
    for sample in raw["samples"]:
        sample["thermal_zones"] = [
            {"zone": f"thermal_zone{index}:{family}-thermal",
             "temp_c": None if family.startswith("cv") else 45.0}
            for index, family in enumerate(FAMILIES)]
    return raw


def test_nano_coverage_accepts_only_explicit_opt_in() -> None:
    # Given the same six real readings and three explicitly unavailable CV zones.
    opted = nano_capture()
    strict = nano_capture()
    strict.pop("thermal_policy")
    # When the actual reporter independently replays both captures.
    default_doc, nano_doc = result(strict), result(opted)
    # Then strict remains blocked, and the approved scope permits thermal coverage.
    assert default_doc["verdict"] == "BLOCKED"
    assert "coverage:thermal" in default_doc["named_gaps"]
    assert nano_doc["verdict"] == "PASS", nano_doc["blocked_on"]


def test_required_families_and_inventory_stay_continuous() -> None:
    # Given each required family failing at an active edge or interior sample.
    for family in ("cpu", "gpu", "soc0", "soc1", "soc2", "tj"):
        for index in (0, 1, -1):
            for fault in ("null", "missing", "rename", "duplicate_family"):
                raw = nano_capture()
                zones = raw["samples"][index]["thermal_zones"]
                zone = zones[FAMILIES.index(family)]
                match fault:
                    case "null":
                        zone["temp_c"] = None
                    case "missing":
                        zones.remove(zone)
                    case "rename":
                        zone["zone"] += "-renamed"
                    case "duplicate_family":
                        zones.append({**zone, "zone": "thermal_zone99:" + family + "-thermal"})
                    case unreachable:
                        assert_never(unreachable)
                # When / Then: replay cannot heal missing required readings or identity changes.
                assert result(raw)["verdict"] == "BLOCKED", (family, index, fault)
    for fault in ("missing_cv", "renamed_cv", "extra_zone", "extra_null"):
        raw = nano_capture()
        zones = raw["samples"][1]["thermal_zones"]
        match fault:
            case "missing_cv":
                zones.pop(2)
            case "renamed_cv":
                zones[2]["zone"] += "-renamed"
            case "extra_zone":
                zones.append({"zone": "thermal_zone99:other", "temp_c": 46.0})
            case "extra_null":
                for sample in raw["samples"]:
                    sample["thermal_zones"].append({"zone": "thermal_zone99:other", "temp_c": None})
            case unreachable:
                assert_never(unreachable)
        assert result(raw)["verdict"] == "BLOCKED", fault


def test_real_cv_readings_and_unavailability_survive_replay() -> None:
    # Given actual CV readings mixed with unavailable observations.
    raw = nano_capture()
    raw["samples"][1]["thermal_zones"][2]["temp_c"] = 63.25
    raw["samples"][2]["thermal_zones"][2]["temp_c"] = -2.0
    before = copy.deepcopy(raw)
    # When the real reporter reduces all readings, without changing raw evidence.
    doc = result(raw)
    # Then numeric CV temperatures contribute to peaks and unavailable counts stay explicit.
    assert raw == before
    assert doc["thermal_state"]["peak_c"] == 63.25
    assert len(doc["thermal_state"]["zones"]) == 9
    scope = doc["measurement_coverage"]["thermal_scope"]
    assert scope["policy"] == POLICY
    counts = {zone["zone"]: zone for zone in scope["zones"]}
    assert counts["thermal_zone2:cv0-thermal"]["numeric"] == 2
    assert counts["thermal_zone2:cv0-thermal"]["unavailable"] == 3
    assert counts["thermal_zone3:cv1-thermal"]["numeric"] == 0
    parsed = coverage.parse_coverage(doc["measurement_coverage"])
    assert coverage.coverage_gaps(parsed, 20.0) == ()


def test_wrong_model_policy_and_nonfinite_readings_reject() -> None:
    for section, field, value in (("thermal_policy", "profile", "unknown"),
                                  ("thermal_policy", "model", "other Jetson"),
                                  ("thermal_policy", "approval", "inferred"),
                                  ("thermal_policy", "scope", "all-zones"),
                                  ("target_hardware_state", "model", "other Jetson"),
                                  ("target_hardware_state", "model", None)):
        # Given a forged selection or missing model binding.
        raw = nano_capture()
        raw[section][field] = value
        # When / Then: the typed capture boundary rejects it before reduction.
        try:
            coverage.parse_capture(raw)
        except coverage.CoverageError:
            continue
        raise AssertionError((section, field, value))
    for index in range(9):
        for value in (float("nan"), float("inf"), float("-inf")):
            raw = nano_capture()
            raw["samples"][1]["thermal_zones"][index]["temp_c"] = value
            try:
                coverage.parse_capture(raw)
            except coverage.CoverageError:
                continue
            raise AssertionError("nonfinite thermal number accepted")


def test_collector_retains_all_raw_zones_and_guards_before_launch() -> None:
    # Given real filesystem reads with three unavailable temp files and all nine types.
    with tempfile.TemporaryDirectory(prefix="nano-thermal-") as directory:
        paths = [Path(directory) / f"thermal_zone{index}" for index in range(9)]
        for path, family in zip(paths, FAMILIES):
            path.mkdir()
            (path / "type").write_text(family + "-thermal", encoding="utf-8")
            if not family.startswith("cv"):
                (path / "temp").write_text("45000", encoding="utf-8")
        with patch.object(collector.glob, "glob", return_value=[str(path) for path in paths]):
            initial = collector._thermal_zones()
            (paths[2] / "temp").write_text("63250", encoding="utf-8")
            # When the existing sampler sees a real CV reading on the next sample.
            observed = collector._thermal_zones(initial)
        # Then every zone remains raw, and only the actual CV reading becomes numeric.
        assert len(initial) == len(observed) == 9
        assert [zone.temp_c for zone in initial][2:5] == [None, None, None]
        assert observed[2].temp_c == 63.25 and observed[3].temp_c is None
    for model in (None, "unknown", "NVIDIA Jetson AGX Orin", MODEL + " other"):
        with (patch.object(collector, "_target_hardware_state", return_value={"model": model}),
              patch.object(collector.subprocess, "Popen") as launch):
            try:
                collector.collect(argparse.Namespace(thermal_policy=PROFILE))
            except coverage.CoverageError:
                launch.assert_not_called()
                continue
            raise AssertionError("wrong model launched a workload")


def test_budget_and_terminal_failures_still_fail() -> None:
    for fault in ("rss", "cpu", "av", "audio", "growth", "crash", "terminal", "freeze"):
        # Given approved thermal scope alongside a real existing run/budget failure.
        raw = nano_capture(15 if fault == "freeze" else 20)
        match fault:
            case "rss": raw["samples"][1]["receiver_rss_mb"] = 251.0
            case "cpu": raw["samples"][-1]["receiver_cpu_ticks"] = 2000
            case "av": raw["events"]["av_sync_ms"] = 50.01
            case "audio": raw["events"]["audio_latency_ms"] = 200.01
            case "growth": raw["samples"][-1]["receiver_rss_mb"] = 110.0
            case "crash": raw["crashes"] = 1
            case "terminal": raw["terminal_errors"] = [{"event": "err", "code": "fixture"}]
            case "freeze":
                for sample in raw["samples"]:
                    sample["av_pairs"] = 1
            case unreachable:
                assert_never(unreachable)
        # When / Then: the availability exception never suppresses definite FAIL.
        assert result(raw)["verdict"] == "FAIL", fault


def test_collector_discloses_policy_and_latches_disappearing_cv_inventory() -> None:
    # Given approved hardware and otherwise continuous progress on the existing virtual clock.
    clock, logs = [0], []
    class LoggingProcess(Process):
        def __init__(self, command, *, stdout, **kwargs):
            logs.append(stdout)
            super().__init__(command, stdout=stdout, **kwargs)
    def sleep(seconds: float) -> None:
        clock[0] += round(seconds * 1e9)
        for event in ({"event": "active", "startup_ms": 0},
                      {"event": "video_sent", "units": clock[0]},
                      {"event": "audio_chunks", "count": clock[0]},
                      {"event": "video_presented", "scheduled_ns": clock[0], "completed_ns": clock[0]},
                      {"event": "audio_write_completed", "scheduled_ns": clock[0],
                       "scheduled_end_ns": clock[0] + 1, "completed_ns": clock[0]}):
            logs[0].write(json.dumps(event) + "\n")
        logs[0].flush()
    zones = tuple(coverage.ThermalZone(zone["zone"], zone["temp_c"])
                  for zone in nano_capture()["samples"][0]["thermal_zones"])
    present = {zone.zone: "fixture" for zone in zones}
    changed = {name: path for name, path in present.items() if "cv0" not in name}
    with tempfile.TemporaryDirectory(prefix="nano-collector-") as directory:
        args = argparse.Namespace(receiver="fixture", display=None, no_display=True, port="fixture",
                                  duration_seconds=3, interval=1, startup_timeout_seconds=5,
                                  store=None, state_dir=directory, thermal_policy=PROFILE)
        with (patch.object(collector, "_target_hardware_state", return_value={"model": MODEL}),
              patch.object(collector.time, "monotonic_ns", side_effect=lambda: clock[0]),
              patch.object(collector.time, "sleep", side_effect=sleep),
              patch.object(collector.subprocess, "Popen", LoggingProcess),
              patch.object(collector, "_thermal_zones", return_value=zones),
              patch.object(collector, "_thermal_inventory", side_effect=(present, changed, present, present))):
            # When a CV directory disappears but its retained raw reading is still null.
            raw = collector.collect(args)
    # Then the explicit policy survives capture, but inventory loss cannot qualify.
    assert raw["thermal_policy"] == POLICY
    assert len(coverage.parse_capture(json.loads(json.dumps(raw))).samples[1].thermal_zones) == 9
    assert "thermal_inventory_changed" in raw["invalid_telemetry"]
    assert result(json.loads(json.dumps(raw)))["verdict"] == "BLOCKED"


def test_export_cannot_relabel_nano_as_strict_or_erase_availability() -> None:
    base = result(nano_capture())
    for fault in ("strict", "strip_scope", "strip_both", "unknown", "wrong_model", "counts", "peak"):
        # Given a sanitized result changed independently after capture removal.
        doc = copy.deepcopy(base)
        exported = doc["measurement_coverage"]
        match fault:
            case "strict": exported["policy"] = coverage.POLICY
            case "strip_scope": exported.pop("thermal_scope")
            case "strip_both":
                exported["policy"] = coverage.POLICY
                exported.pop("thermal_scope")
            case "unknown": exported["thermal_scope"]["policy"]["profile"] = "unknown"
            case "wrong_model": doc["provenance"]["target_hardware_state"]["model"] = "other Jetson"
            case "counts": exported["thermal_scope"]["zones"][0]["numeric"] -= 1
            case "peak": doc["thermal_state"]["zones"][2]["temp_c"] = 45.0
            case unreachable:
                assert_never(unreachable)
        # When / Then: independent schema/semantic validation rejects the forged scope.
        assert schema.validate(doc, schema.load_schema(schema.DEFAULT_SCHEMA)), fault


def test_export_rejects_strict_relabeling_when_cv_peaks_are_numeric() -> None:
    # Given a capture-free PASS with each CV zone measured only once in five samples.
    raw = nano_capture()
    for zone in raw["samples"][1]["thermal_zones"]:
        if ":cv" in zone["zone"]:
            zone["temp_c"] = 47.0
    doc = result(raw)
    assert doc["verdict"] == "PASS" and "capture" not in doc
    assert not schema.validate(doc, schema.load_schema(schema.DEFAULT_SCHEMA))
    # When the availability scope is removed and its policy substituted with strict.
    doc["measurement_coverage"]["policy"] = coverage.POLICY
    doc["measurement_coverage"].pop("thermal_scope")
    # Then numeric peaks cannot erase the result's reliance on the Nano exception.
    assert schema.validate(doc, schema.load_schema(schema.DEFAULT_SCHEMA))


def test_offline_shell_preserves_selection_and_cannot_opt_in_replay() -> None:
    # Given two offline fixtures differing only in explicit approved selection.
    with tempfile.TemporaryDirectory(prefix="nano-policy-cli-") as directory:
        for selected, expected in ((False, 3), (True, 0)):
            raw = nano_capture()
            if not selected:
                raw.pop("thermal_policy")
            source, output = Path(directory) / "input.json", Path(directory) / "output.json"
            source.write_text(json.dumps(raw), encoding="utf-8")
            command = ["bash", str(ROOT / "tools/perf/aa-projection-perf.sh"), "--mode", "wired",
                       "--capture", str(source), "--out", str(output)]
            # When the real shell launcher invokes its real reporter offline.
            process = subprocess.run(command, capture_output=True, text=True, timeout=15, check=False)
            # Then strict BLOCKED and scoped PASS retain their independent export semantics.
            assert process.returncode == expected, process.stderr
            doc = json.loads(output.read_text(encoding="utf-8"))
            assert doc["qualification_status"] == ("OFFLINE_ONLY" if selected else "INCOMPLETE")
            assert not schema.validate(doc, schema.load_schema(schema.DEFAULT_SCHEMA))
            wrong = "strict-all-zones" if selected else PROFILE
            process = subprocess.run(command + ["--thermal-policy", wrong], capture_output=True,
                                     text=True, timeout=15, check=False)
            assert process.returncode == 4, process.stderr


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} Nano thermal policy tests")
