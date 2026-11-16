#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/perf/test_v3_av_contract.py
"""v3 soak results: the signed A/V contract, the moving-map workload, v2 history (#137)."""
from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import aa_perf_report as reporter  # noqa: E402
import aa_perf_schema as schema  # noqa: E402
from test_v2_coverage import NS, synthetic  # noqa: E402

FRAME_NS = 33_333_333
SLOT_NS = 42_666_666


def v3(seconds: int = 1800, skew_ms=lambda at_s: 12.0, audio=(0.0, None),
       workload: dict | None = None) -> dict:
    """A v3 capture: the v2 synthetic evidence plus a 30 fps signed series and audio.

    ``skew_ms(at_s)`` gives each pair's signed skew; ``audio`` is the (from, until)
    seconds the audio slots cover (until None: to the end); None means no audio.
    """
    raw = synthetic(seconds)
    raw["schema"] = "aa-projection-perf-capture/3"
    times = list(range(0, seconds * NS + 1, FRAME_NS))
    raw["av_series"] = {"pair_t_ns": [], "pair_skew_ns": [], "audio_intervals_ns": []}
    if audio is not None:
        lo = int(audio[0] * NS)
        hi = seconds * NS if audio[1] is None else int(audio[1] * NS)
        raw["av_series"]["audio_intervals_ns"] = [[lo, hi]]
        inside = [t for t in times if lo <= t < hi]
        raw["av_series"]["pair_t_ns"] = inside
        raw["av_series"]["pair_skew_ns"] = [int(skew_ms(t / NS) * 1e6) for t in inside]
    raw["workload"] = workload or {"kind": "moving-map", "media_relaunches": 0, "drags": seconds,
                                   "stimulus_started": True}
    return raw


def result(raw: dict, *, live: bool = False) -> dict:
    raw = copy.deepcopy(raw)
    if live:
        raw["measurement_origin"] = "live"
    return reporter.build_result(raw, duration_requested=None, now="2026-11-16T00:00:00Z",
                                 capture_path=None, capture_sha256=None)


def violations(doc: dict) -> set[str]:
    return {v["metric"] for v in doc["violations"]}


def test_healthy_moving_map_soak_passes_under_v3() -> None:
    for seconds in (1800, 7200):
        doc = result(v3(seconds))
        assert doc["schema"] == "aa-projection-perf/3", doc["schema"]
        assert doc["verdict"] == "PASS", (doc["violations"], doc["blocked_on"])
        assert not schema.validate(doc, schema.load_schema(schema.SCHEMA_V3))
        assert doc["av_contract"]["passed"] is True and doc["av_contract"]["valid"] is True
        assert doc["metrics"]["av_p99_abs_ms"]["value"] == 12.0
        assert doc["metrics"]["av_coverage_pct"]["status"] == "PASS"
        assert doc["metrics"]["av_sync_ms"]["gated"] is False  # informational in v3
        assert doc["budgets"] == reporter.BUDGETS and "av_sync_ms_max" not in doc["budgets"]
        assert doc["workload"]["kind"] == "moving-map"


def test_no_audio_soak_fails() -> None:
    # Given a measured active window but no audio at all.
    doc = result(v3(1800, audio=None))
    # Then the soak FAILs on coverage (never n/a), the skew metrics are empty.
    assert doc["verdict"] == "FAIL", doc["verdict"]
    assert violations(doc) == {"av_coverage_pct"}, doc["violations"]
    assert doc["metrics"]["av_coverage_pct"]["value"] == 0.0
    assert doc["av_contract"]["pairs"] == 0 and doc["av_contract"]["passed"] is False


def test_each_clause_fails_alone() -> None:
    cases = {
        # 5.5 s at +45 ms (and -45 ms): under 1 % of pairs, one window mean over 40.
        "av_window_mean_abs_ms": [lambda at: 45.0 if 600 <= at < 605.5 else 12.0,
                                  lambda at: -45.0 if 600 <= at < 605.5 else 12.0],
        # Every 40th frame 60 ms late: p99 over 50 ms, means near 13 ms.
        "av_p99_abs_ms": [lambda at: 60.0 if round(at * NS / FRAME_NS) % 40 == 0 else 12.0],
        # One frame at 95 ms: over the 90 ms cap.
        "av_max_abs_ms": [lambda at: 95.0 if 900.0 <= at < 900.04 else 12.0],
    }
    for metric, functions in cases.items():
        for function in functions:
            doc = result(v3(1800, skew_ms=function))
            assert doc["verdict"] == "FAIL", (metric, doc["blocked_on"])
            assert violations(doc) == {metric}, (metric, doc["violations"])
    # Audio for the last 80 % only: coverage alone fails; 92 % passes.
    assert violations(result(v3(1800, audio=(360.0, None)))) == {"av_coverage_pct"}
    assert result(v3(1800, audio=(144.0, None)))["verdict"] == "PASS"
    # In-bound controls: 39 ms sustained and an 85 ms single frame pass.
    assert result(v3(1800, skew_ms=lambda at: 39.0 if 600 <= at < 620 else 12.0))["verdict"] == "PASS"
    assert result(v3(1800, skew_ms=lambda at: 85.0 if 900.0 <= at < 900.04 else 12.0))["verdict"] == "PASS"


def test_invalid_av_telemetry_still_blocks() -> None:
    # The soak's strict rule is kept: an invalid A/V endpoint makes A/V unavailable.
    raw = v3(1800)
    raw["invalid_telemetry"] = ["av_sync_ms"]
    doc = result(raw)
    assert doc["verdict"] == "BLOCKED", doc["verdict"]
    assert doc["av_contract"]["valid"] is False
    assert "av_p99_abs_ms" in doc["blocked_on"] and "invalid_telemetry" in doc["blocked_on"]


def test_live_v3_requires_the_moving_map_workload() -> None:
    doc = result(v3(1800, workload={"kind": "none"}), live=True)
    assert doc["verdict"] == "BLOCKED", doc["verdict"]
    assert "run_evidence:moving_map_workload" in doc["blocked_on"], doc["blocked_on"]
    assert "run_evidence:moving_map_workload" not in (result(v3(1800), live=True)["blocked_on"] or "")


def test_live_v3_requires_proof_that_the_stimulus_ran() -> None:
    # The workload label alone is not evidence: started, enough drags, media count.
    base = {"kind": "moving-map", "media_relaunches": 0, "drags": 1800, "stimulus_started": True}
    cases = {
        "run_evidence:moving_map_stimulus": [{**base, "stimulus_started": False, "drags": 0},
                                             {**base, "stimulus_started": False},
                                             {"kind": "moving-map"}],
        # 1800 s at the 0.25 Hz floor needs 450 drags (about 0.65 Hz nominal).
        "run_evidence:moving_map_drags": [{**base, "drags": 0}, {**base, "drags": 449}],
    }
    for reason, workloads in cases.items():
        for workload in workloads:
            doc = result(v3(1800, workload=workload), live=True)
            assert doc["verdict"] == "BLOCKED", (workload, doc["verdict"])
            assert reason in doc["blocked_on"], (workload, doc["blocked_on"])
            # The result records the evidence it saw, every field present.
            assert set(doc["workload"]) == {"kind", "media_relaunches", "drags", "stimulus_started"}
    ok = result(v3(1800, workload={**base, "drags": 450}), live=True)
    assert "moving_map" not in (ok["blocked_on"] or ""), ok["blocked_on"]
    # The v3 schema requires the evidence fields for a moving-map record.
    bare = copy.deepcopy(result(v3(60)))
    bare["workload"] = {"kind": "moving-map"}
    assert schema.validate(bare, schema.load_schema(schema.SCHEMA_V3))


def test_malformed_series_is_a_contract_violation() -> None:
    for change in ({"pair_t_ns": [1, 2], "pair_skew_ns": [1]},
                   {"pair_t_ns": [1.5], "pair_skew_ns": [1]},
                   {"audio_intervals_ns": [[1]]}, {"audio_intervals_ns": [["a", "b"]]}):
        raw = v3(60)
        raw["av_series"].update(change)
        try:
            result(raw)
        except reporter.ContractError:
            continue
        raise AssertionError(f"malformed av_series accepted: {change}")


def test_v2_history_keeps_its_contract() -> None:
    # A v2 capture still reports as v2 under BUDGETS_V2 (A/V max <= 50 ms, gated).
    raw = synthetic(1800)
    doc = result(raw)
    assert doc["schema"] == "aa-projection-perf/2" and doc["verdict"] == "PASS"
    assert doc["budgets"] == reporter.BUDGETS_V2
    assert doc["metrics"]["av_sync_ms"]["gated"] is True
    assert "av_contract" not in doc and "workload" not in doc
    assert not schema.validate(doc, schema.load_schema(schema.DEFAULT_SCHEMA))
    over = copy.deepcopy(raw)
    over["events"]["av_sync_ms"] = 50.5
    assert violations(result(over)) == {"av_sync_ms"}
    # A v3 document never validates as v2 and vice versa.
    v3_doc = result(v3(60))
    assert schema.validate(v3_doc, schema.load_schema(schema.DEFAULT_SCHEMA))
    assert schema.validate(doc, schema.load_schema(schema.SCHEMA_V3))


def test_schema_pins_the_v3_budgets_and_validate_auto() -> None:
    doc = result(v3(60))
    loaded = schema.load_schema(schema.SCHEMA_V3)
    for name in ("av_window_mean_abs_ms_max", "av_p99_abs_ms_max", "av_max_abs_ms_max",
                 "av_coverage_pct_min"):
        weakened = copy.deepcopy(doc)
        weakened["budgets"][name] += 1
        assert schema.validate(weakened, loaded), f"v3 schema accepted a weakened {name}"
    loosened = copy.deepcopy(doc)
    loosened["av_contract"]["limits"]["p99_abs_ms"] = 60.0
    assert schema.validate(loosened, loaded), "v3 schema accepted loosened contract limits"
    with tempfile.TemporaryDirectory(prefix="perf-v3-") as directory:
        for name, document in (("v3.json", doc), ("v2.json", result(synthetic(60)))):
            path = Path(directory) / name
            path.write_text(json.dumps(document))
            check = subprocess.run([sys.executable, "-B", str(ROOT / "tools/perf/aa_perf_schema.py"),
                                    "validate-auto", str(path)], capture_output=True, text=True)
            assert check.returncode == 0, check.stderr


def test_reporter_cli_dispatches_by_capture_version() -> None:
    with tempfile.TemporaryDirectory(prefix="perf-v3-cli-") as directory:
        for raw, version, code in ((v3(60), "aa-projection-perf/3", 0),
                                   (synthetic(60), "aa-projection-perf/2", 0),
                                   (v3(60, audio=None), "aa-projection-perf/3", 1)):
            capture, out = Path(directory) / "capture.json", Path(directory) / "out.json"
            capture.write_text(json.dumps(raw))
            run = subprocess.run(["bash", str(ROOT / "tools/perf/aa-projection-perf.sh"), "--mode",
                                  "wired", "--capture", str(capture), "--out", str(out)],
                                 capture_output=True, text=True, timeout=60)
            assert run.returncode == code, (version, run.returncode, run.stderr)
            assert json.loads(out.read_text())["schema"] == version


if __name__ == "__main__":
    tests = [value for name, value in list(globals().items()) if name.startswith("test_")]
    for test in tests:
        test()
    print(f"PASS {len(tests)} v3 signed A/V soak contract tests")
