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
    raw["workload"] = workload or moving_map(seconds)
    raw["concurrent_builds"] = []
    return raw


DRAG_NS = 1_500_000_000  # a drag every 1.5 s, as the live stimulus does


def moving_map(seconds: int, **change) -> dict:
    """A healthy moving-map record: drags every 1.5 s past the window's end."""
    times = list(range(DRAG_NS // 2, (seconds + 10) * NS, DRAG_NS))
    return {"kind": "moving-map", "media_relaunches": 0, "drags": len(times),
            "stimulus_started": True, "phone_failure": None, "drag_t_ns": times,
            "cleanup_failures": [], **change}


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
        # Two frames at 95 ms in 1800 s: over the allowance of one excursion.
        "av_excursions": [spikes(95.0, 27_000, 40_000), spikes(-95.0, 27_000, 40_000)],
        # One frame at 150.1 ms: a single excursion is allowed, the hard cap is not.
        "av_hard_cap_pairs": [spikes(150.1, 27_000), spikes(-150.1, 27_000)],
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
    assert result(v3(1800, skew_ms=spikes(85.0, 27_000)))["verdict"] == "PASS"


def spikes(ms: float, *frames: int):
    """skew_ms(at): ``ms`` on the given frame indices (30 fps), 12 ms elsewhere."""
    hit = set(frames)
    return lambda at: ms if round(at * NS / FRAME_NS) in hit else 12.0


def test_soak_max_is_a_rate_of_excursions_with_a_hard_cap() -> None:
    # Owner decision 2026-11-17: at most floor(active s / 1800) pairs over 90 ms,
    # none over 150 ms; max |skew| is informational.
    cases = [  # (seconds, skew, verdict, excursions, allowance)
        (1800, lambda at: 12.0, "PASS", 0, 1),
        (1800, spikes(95.0, 11_577), "PASS", 1, 1),
        (1800, spikes(95.0, 11_577, 30_000), "FAIL", 2, 1),
        # Consecutive pairs of one spike count separately.
        (1800, spikes(95.0, 11_577, 11_578), "FAIL", 2, 1),
        (7200, spikes(95.0, 10_000, 50_000, 100_000, 200_000), "PASS", 4, 4),
        (7200, spikes(-95.0, 10_000, 50_000, 100_000, 150_000, 200_000), "FAIL", 5, 4),
        (1800, spikes(150.1, 11_577), "FAIL", 1, 1),
        (1800, spikes(150.0, 11_577), "PASS", 1, 1),  # the cap itself is allowed
    ]
    for seconds, skew, verdict, excursions, allowance in cases:
        doc = result(v3(seconds, skew_ms=skew))
        contract = doc["av_contract"]
        label = (seconds, verdict, excursions)
        assert doc["verdict"] == verdict, (label, doc["violations"], doc["blocked_on"])
        assert not schema.validate(doc, schema.load_schema(schema.SCHEMA_V3)), label
        assert contract["max_clause"] == "soak_rate" and "max_abs" not in contract["clauses"], label
        assert (contract["excursions"], contract["excursion_allowance"]) == (excursions, allowance), label
        assert doc["metrics"]["av_excursions"]["value"] == excursions, label
        assert doc["metrics"]["av_excursions"]["budget_max"] == allowance, label
        assert doc["metrics"]["av_max_abs_ms"]["gated"] is False, label  # informational
        assert len(contract["excursion_pairs"]) == excursions, label
        if verdict == "FAIL":
            assert violations(doc) <= {"av_excursions", "av_hard_cap_pairs"}, (label, doc["violations"])
    # Every excursion is reported with its offset into the window and its value.
    doc = result(v3(1800, skew_ms=spikes(95.0, 11_577)))
    [pair] = doc["av_contract"]["excursion_pairs"]
    assert abs(pair["offset_s"] - 11_577 * FRAME_NS / NS) < 1e-9 and pair["skew_ms"] == 95.0, pair
    assert doc["metrics"]["av_max_abs_ms"]["value"] == 95.0
    # The 150.1 ms pair fails the hard cap alone (one excursion is allowed).
    doc = result(v3(1800, skew_ms=spikes(150.1, 11_577)))
    assert violations(doc) == {"av_hard_cap_pairs"}, doc["violations"]
    assert doc["av_contract"]["clauses"] == {"coverage": True, "window_mean": True, "p99_abs": True,
                                             "excursions": True, "hard_cap": False}


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
    base = moving_map(1800, drags=1800)
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
            assert set(doc["workload"]) == {"kind", "media_relaunches", "drags", "stimulus_started",
                                            "phone_failure", "max_drag_gap_s", "cleanup_failures"}
    ok = result(v3(1800, workload={**base, "drags": 450}), live=True)
    assert "moving_map" not in (ok["blocked_on"] or ""), ok["blocked_on"]
    # The v3 schema requires the evidence fields for a moving-map record.
    bare = copy.deepcopy(result(v3(60)))
    bare["workload"] = {"kind": "moving-map"}
    assert schema.validate(bare, schema.load_schema(schema.SCHEMA_V3))


def test_live_v3_blocks_a_failed_phone_command() -> None:
    # Given a stage whose media relaunch (or setup) adb command failed: the
    # harness lost the phone, the moving map is not proven -> BLOCKED, never PASS.
    for failure in ("media: exit 1: error: device offline", "maps: Error: Activity not started"):
        doc = result(v3(1800, workload=moving_map(1800, phone_failure=failure)), live=True)
        assert doc["verdict"] == "BLOCKED", (failure, doc["verdict"])
        assert "run_evidence:moving_map_phone" in doc["blocked_on"], doc["blocked_on"]
        assert doc["workload"]["phone_failure"] == failure
    # A record that does not say whether a phone command failed proves nothing.
    record = moving_map(1800)
    del record["phone_failure"]
    doc = result(v3(1800, workload=record), live=True)
    assert "run_evidence:moving_map_phone" in doc["blocked_on"], doc["blocked_on"]
    assert doc["workload"]["phone_failure"] == "not recorded"


def test_live_v3_requires_drags_across_the_whole_window() -> None:
    # The review's case: plenty of early drags (1800 in the first 1200 s, over
    # the 0.25 Hz aggregate floor), then the control pipe is lost for 600 s.
    early = [t for t in range(0, 1200 * NS, 666_666_666)][:1800]
    doc = result(v3(1800, workload=moving_map(1800, drags=len(early), drag_t_ns=early)), live=True)
    assert len(early) >= 0.25 * 1800
    assert doc["verdict"] == "BLOCKED", doc["verdict"]
    assert "run_evidence:moving_map_drag_gap" in doc["blocked_on"], doc["blocked_on"]
    assert doc["workload"]["max_drag_gap_s"] > 590
    # One hole just over MAX_DRAG_GAP_S blocks; one just under passes.
    for hole_s, blocked in ((10.5, True), (9.5, False)):
        times = [t for t in moving_map(1800)["drag_t_ns"] if not 900 * NS < t < (900 + hole_s) * NS]
        times = sorted({*times, 900 * NS, int((900 + hole_s) * NS)})
        doc = result(v3(1800, workload=moving_map(1800, drags=len(times), drag_t_ns=times)), live=True)
        assert abs(doc["workload"]["max_drag_gap_s"] - hole_s) < 1e-9, doc["workload"]
        assert ("run_evidence:moving_map_drag_gap" in (doc["blocked_on"] or "")) is blocked, \
            (hole_s, doc["blocked_on"])
    # The window's edges count: no drag in the last 11 s blocks.
    tail = [t for t in moving_map(1800)["drag_t_ns"] if t < 1789 * NS]
    doc = result(v3(1800, workload=moving_map(1800, drags=len(tail), drag_t_ns=tail)), live=True)
    assert "run_evidence:moving_map_drag_gap" in doc["blocked_on"], doc["blocked_on"]
    # Missing or malformed drag times are no continuity evidence.
    for bad in (None, [3, 2], [1.5], "x"):
        record = moving_map(1800)
        record["drag_t_ns"] = bad
        if bad is None:
            del record["drag_t_ns"]
        doc = result(v3(1800, workload=record), live=True)
        assert "run_evidence:moving_map_drag_gap" in doc["blocked_on"], (bad, doc["blocked_on"])
        assert doc["workload"]["max_drag_gap_s"] is None
    assert result(v3(1800), live=True)["workload"]["max_drag_gap_s"] == 1.5


def test_a_cleanup_failure_blocks_the_stage_by_name() -> None:
    # The phone restore must be shown complete: a player left running (or a USB
    # mode not restored) BLOCKS a stage that would otherwise pass, by name, so
    # the chain stops before the next stage.
    failure = "media_stop_unverified: org.example.player after 3 attempts: still running (pid 1)"
    cases = {
        (failure,): "cleanup:media_not_stopped",
        ("media_stop_unavailable: no media component, the player is unknown",): "cleanup:media_not_stopped",
        ("usb_reset_failed: exit 1",): "cleanup:usb_not_restored",
        ("phone_restore_not_reached",): "cleanup:phone_restore_not_reached",
    }
    for failures, reason in cases.items():
        doc = result(v3(1800, workload=moving_map(1800, cleanup_failures=list(failures))), live=True)
        assert doc["verdict"] == "BLOCKED" and reason in doc["blocked_on"], (failures, doc["blocked_on"])
        assert doc["workload"]["cleanup_failures"] == list(failures)
        assert not schema.validate(doc, schema.load_schema(schema.SCHEMA_V3))
    assert result(v3(1800), live=True)["verdict"] == "PASS"
    # A capture that does not record its restore is not proven clean either.
    record = moving_map(1800)
    del record["cleanup_failures"]
    doc = result(v3(1800, workload=record), live=True)
    assert doc["workload"]["cleanup_failures"] == ["cleanup not recorded"]
    assert doc["verdict"] == "BLOCKED" and "cleanup:not_recorded" in doc["blocked_on"], doc["blocked_on"]
    # A measured budget miss stays a FAIL (a BLOCKED reason never hides it).
    raw = v3(1800, audio=None, workload=moving_map(1800, cleanup_failures=[failure]))
    assert result(raw, live=True)["verdict"] == "FAIL"


def test_live_v3_blocks_a_build_during_the_stage() -> None:
    # Given a capture whose on-target watch saw a compiler during the stage.
    raw = v3(1800)
    raw["concurrent_builds"] = [{"t_ns": 600 * NS, "processes": ["4242 cc1plus", "4240 ninja"]}]
    doc = result(raw, live=True)
    # Then the stage is BLOCKED (interference), with the evidence in the result.
    assert doc["verdict"] == "BLOCKED", doc["verdict"]
    assert "interference:concurrent_build" in doc["blocked_on"], doc["blocked_on"]
    assert doc["run_status"]["concurrent_builds"] == [
        {"t_s": 600.0, "processes": ["4242 cc1plus", "4240 ninja"]}]
    assert not schema.validate(doc, schema.load_schema(schema.SCHEMA_V3))
    # A capture without the watch is not proven quiet.
    del raw["concurrent_builds"]
    doc = result(raw, live=True)
    assert "run_evidence:concurrent_build_watch" in doc["blocked_on"], doc["blocked_on"]
    assert doc["run_status"]["concurrent_builds"] is None
    # A measured budget miss is still a FAIL under interference (never hidden).
    raw = v3(1800, audio=None)
    raw["concurrent_builds"] = [{"t_ns": 600 * NS, "processes": ["4242 cc1plus"]}]
    assert result(raw, live=True)["verdict"] == "FAIL"
    # The quiet control: an empty watch.
    assert result(v3(1800), live=True)["run_status"]["concurrent_builds"] == []


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
    for name in ("av_window_mean_abs_ms_max", "av_p99_abs_ms_max", "av_excursion_ms",
                 "av_excursions_per_s", "av_hard_cap_ms", "av_coverage_pct_min"):
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


def test_evaluate_window_is_half_open_at_its_end() -> None:
    # evaluate() documents [start, end): a pair at end_ns belongs to the next
    # window, one at start_ns to this one (the review of #152).
    import aa_av_contract as contract
    start, end = 0, 1800 * NS
    spike = 500.0  # ms: over the hard cap, so counting it fails the soak clause
    cover = [(start, end)]
    steady = [(t, 5.0) for t in range(0, end, FRAME_NS)]
    at_end = contract.evaluate(steady + [(end, spike)], cover, start, end, max_clause="soak_rate")
    assert at_end["pairs"] == len(steady) and at_end["over_hard_cap"] == 0 and at_end["passed"], at_end
    just_inside = contract.evaluate(steady + [(end - 1, spike)], cover, start, end, max_clause="soak_rate")
    assert just_inside["pairs"] == len(steady) + 1 and just_inside["over_hard_cap"] == 1
    assert not just_inside["passed"]
    at_start = contract.evaluate([(start, spike)] + steady[1:], cover, start, end, max_clause="soak_rate")
    assert at_start["over_hard_cap"] == 1, "a pair exactly at start_ns is inside"


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
