#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Deterministic v2 contract regressions; no hardware or qualification claims."""
from __future__ import annotations

import copy
import json
import subprocess
import sys
import tempfile
from dataclasses import asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/perf"))
import aa_measurement_coverage as coverage
import aa_perf_report as reporter
import aa_perf_schema as schema

NS = 1_000_000_000


def synthetic(seconds: int = 1800) -> dict:
    samples = []
    blocks = []
    for index, second in enumerate(range(0, seconds + 1, 5)):
        stamp = second * NS
        samples.append({"t_ns": stamp, "receiver_rss_mb": 100.0,
                        "receiver_cpu_ticks": second * 10,
                        "thermal_zones": [{"zone": "thermal_zone0:CPU", "temp_c": 45.0}],
                        "projection_active": True,
                        **{key: index + 1 for key in ("video_units", "audio_chunks",
                           "video_presentations", "audio_writes", "av_pairs")}})
        if index:
            stream = {"count": 1, "first_ns": stamp, "last_ns": stamp,
                      "max_internal_gap_ns": 0, "begin_ns": stamp - 40_000_000, "bytes": 0}
            blocks.append({"start_ns": stamp - 5 * NS, "end_ns": stamp,
                           "ipc_consumption": {**stream, "begin_ns": stamp - 5 * NS,
                                               "bytes": 4096},
                           "ipc_matches": {**stream, "begin_ns": stamp - 1_000_000},
                           "decode": dict(stream)})
    return {"schema": "aa-projection-perf-capture/2", "mode": "wired",
            "monotonic_origin_ns": 123 * NS, "cpu_ticks_per_second": 100,
            "duration_seconds": seconds, "observed_duration_seconds": seconds,
            "active_projection_seconds": seconds, "measurement_origin": "offline",
            "crashes": 0, "early_exit": False, "receiver_exit_code": 0,
            "receiver_stopped_by_collector": True, "display_survived": True,
            "display_connected": True, "terminal_errors": [], "invalid_telemetry": [],
            "samples": samples, "video_evidence": {"blocks": blocks,
                "ipc_latency_ns": [1_000_000] * min(4096, len(blocks)),
                "decode_latency_ns": [40_000_000] * min(4096, len(blocks))},
            "events": {"frame_drops": 0, "av_sync_ms": 12.0, "audio_latency_ms": 40.0,
                       "helper_rss_mb": None, "startup_ms": 0,
                       "video_units": len(samples), "audio_chunks": len(samples)},
            "provenance": {"source_digest": "a" * 64, "build_artifact_digest": "b" * 64,
                "configuration_digest": "c" * 64,
                "service_definitions": [{"name": "fixture", "digest": "d" * 64}],
                "dependency_versions": {"fixture": "synthetic-v2"}}}


def result(raw: dict) -> dict:
    return reporter.build_result(raw, duration_requested=None, now="2026-10-17T00:00:00Z",
                                 capture_path=None, capture_sha256=None)


def rejects(raw: dict) -> None:
    try:
        evidence = coverage.parse_capture(raw)
        coverage.reduce_capture(evidence, coverage.Window(0, 20 * NS))
    except coverage.CoverageError:
        return
    raise AssertionError("malformed evidence was accepted")


def valid() -> None:
    for seconds in (1800, 7200):
        # Given complete synthetic active evidence, including decode p95 above 33ms.
        raw = synthetic(seconds)
        # When the actual reporter reduces it.
        doc = result(raw)
        # Then availability passes without restoring a withdrawn latency gate.
        assert doc["verdict"] == "PASS", doc["blocked_on"]
        assert doc["qualification_status"] == "OFFLINE_ONLY"
        assert doc["metrics"]["decode_p95_ms"]["value"] == 40.0
        assert doc["duration_seconds"] == seconds
        parsed = coverage.parse_coverage(doc["measurement_coverage"])
        assert coverage.coverage_gaps(parsed, float(seconds)) == ()
        raw["measurement_origin"] = "live"
        assert result(raw)["qualification_status"] == "COMPLETE_LIVE"


def malformed() -> None:
    for field, value in (("receiver_rss_mb", float("nan")),
                         ("receiver_cpu_ticks", True), ("t_ns", 1.5)):
        raw = synthetic(20)
        raw["samples"][1][field] = value
        rejects(raw)
    for field, value in (("count", True), ("count", -1), ("first_ns", 0),
                         ("max_internal_gap_ns", 1), ("bytes", 1), ("unknown", 0)):
        raw = synthetic(20)
        raw["video_evidence"]["blocks"][0]["decode"][field] = value
        rejects(raw)
    raw = synthetic(20)
    raw["video_evidence"]["decode_latency_ns"].pop()
    rejects(raw)
    raw = synthetic(20)
    raw["video_evidence"]["blocks"][1]["start_ns"] += 1
    rejects(raw)


def missing() -> None:
    for field in ("receiver_rss_mb", "receiver_cpu_ticks", "thermal_zones"):
        # Given exactly one missing active resource observation.
        raw = synthetic(20)
        raw["samples"][1][field] = [] if field == "thermal_zones" else None
        # When reporting, it cannot be hidden by other valid samples.
        doc = result(raw)
        # Then qualification is incomplete.
        assert doc["verdict"] == "BLOCKED", field
        assert doc["qualification_status"] == "INCOMPLETE"
    for stream in coverage.STREAMS:
        raw = synthetic(20)
        for block in raw["video_evidence"]["blocks"]:
            block[stream] = asdict(coverage.StreamBlock(0, None, None, 0, None, 0))
        if stream in ("ipc_consumption", "ipc_matches"):
            if stream == "ipc_consumption":
                for block in raw["video_evidence"]["blocks"]:
                    block["ipc_matches"] = asdict(coverage.StreamBlock(0, None, None, 0, None, 0))
            raw["video_evidence"]["ipc_latency_ns"] = []
        else:
            raw["video_evidence"]["decode_latency_ns"] = []
        doc = result(raw)
        assert doc["verdict"] == "BLOCKED", stream
        assert "coverage:" + stream in doc["named_gaps"]


def precedence() -> None:
    for mutation in ("crash", "short", "rss", "cpu"):
        raw = synthetic(20)
        raw["samples"][1]["thermal_zones"] = []
        if mutation == "crash":
            raw["crashes"] = 1
        elif mutation == "short":
            raw["duration_seconds"] = 1800
        elif mutation == "cpu":
            for sample in raw["samples"]:
                sample["receiver_cpu_ticks"] *= 10
        else:
            raw["samples"][1]["receiver_rss_mb"] = 300
        doc = result(raw)
        assert doc["verdict"] == "FAIL", mutation
    raw = synthetic(20)
    startup = copy.deepcopy(raw["samples"][0])
    startup.update(t_ns=0, receiver_rss_mb=999, receiver_cpu_ticks=0, projection_active=False)
    for sample in raw["samples"]:
        sample["t_ns"] += 5 * NS
    for block in raw["video_evidence"]["blocks"]:
        block["start_ns"] += 5 * NS
        block["end_ns"] += 5 * NS
        for stream in coverage.STREAMS:
            for key in ("first_ns", "last_ns", "begin_ns"):
                block[stream][key] += 5 * NS
    raw["samples"].insert(0, startup)
    assert result(raw)["metrics"]["receiver_rss_peak_mb"]["value"] == 100


def ledger() -> None:
    # Given a ledger with sealed earlier intervals.
    item = coverage.CoverageLedger(0)
    item.advance(5 * NS)
    item.advance(10 * NS)
    item.advance(15 * NS)
    # When replay tries to fill the sealed interval.
    accepted = item.add(coverage.Contribution("decode", 0, 5 * NS, 0))
    # Then old evidence is excluded and the gap survives.
    assert accepted is False
    assert item.finish(15 * NS)[0].decode.count == 0


def summary_mutations() -> None:
    base = result(synthetic(20))["measurement_coverage"]
    mutations = (("sampling", "count", True), ("sampling", "max_gap_ns", 0),
                 ("rss", "valid", 6), ("rss", "missing", 1),
                 ("rss", "first_ns", 1), ("cpu", "measured_interval_ns", 21 * NS),
                 ("cpu", "baseline_valid", False), ("cpu", "max_gap_ns", 1),
                 ("decode", "count", 0), ("decode", "begin_ns", -1),
                 ("decode", "max_gap_ns", 0), ("decode", "bytes", 1),
                 ("quantiles", "decode_retained", 3))
    for section, field, value in mutations:
        # Given a valid result whose summary is changed independently.
        raw = copy.deepcopy(base)
        raw[section][field] = value
        # When the raw capture is no longer available to its reader.
        try:
            coverage.parse_coverage(raw)
        except coverage.CoverageError:
            continue
        # Then contradictory summaries must still be rejected.
        raise AssertionError((section, field, value))


def edges() -> None:
    for edge, expected in ((10 * NS, "PASS"), (10 * NS + 1, "BLOCKED")):
        raw = synthetic(20)
        empty = asdict(coverage.StreamBlock(0, None, None, 0, None, 0))
        for block in raw["video_evidence"]["blocks"]:
            block["decode"] = dict(empty)
        target = 1 if edge == 10 * NS else 2
        raw["video_evidence"]["blocks"][target]["decode"] = {
            "count": 1, "first_ns": edge, "last_ns": edge,
            "max_internal_gap_ns": 0, "begin_ns": edge - 40_000_000, "bytes": 0}
        raw["video_evidence"]["decode_latency_ns"] = [40_000_000]
        assert result(raw)["verdict"] == expected


def weighted_cpu() -> None:
    # Given unequal valid intervals with 10% then 40% CPU.
    raw = synthetic(10)
    raw["samples"][1].update(t_ns=2 * NS, receiver_cpu_ticks=20)
    raw["samples"][2]["receiver_cpu_ticks"] = 340
    raw["video_evidence"]["blocks"][0].update(end_ns=2 * NS)
    raw["video_evidence"]["blocks"][1].update(start_ns=2 * NS)
    for stream in coverage.STREAMS:
        first = raw["video_evidence"]["blocks"][0][stream]
        first.update(first_ns=2 * NS, last_ns=2 * NS,
                     begin_ns=2 * NS - (40_000_000 if stream == "decode" else 1_000_000))
    # When reducing the full active window.
    doc = result(raw)
    # Then sustained CPU is time-weighted, not the 25% subset mean.
    assert doc["metrics"]["receiver_cpu_sustained_pct_one_core"]["value"] == 34
    raw["samples"][1]["receiver_cpu_ticks"] = None
    assert result(raw)["metrics"]["receiver_cpu_sustained_pct_one_core"]["value"] is None


def retained() -> None:
    # Given sealed whole-window summaries larger than the quantile ring.
    raw = synthetic(20)
    for block in raw["video_evidence"]["blocks"]:
        for stream in coverage.STREAMS:
            block[stream]["count"] = 1100
        block["ipc_consumption"]["bytes"] = 1100 * 4096
        block["decode"]["begin_ns"] = block["decode"]["last_ns"] - 100_000_000
    raw["video_evidence"]["ipc_latency_ns"] = [1_000_000] * 4096
    raw["video_evidence"]["decode_latency_ns"] = [34_000_000 + index * 10_000 for index in range(4096)]
    # When reporting bounded percentiles and unbounded sealed counts.
    doc = result(raw)
    # Then counts and coverage survive eviction from the latency ring.
    assert doc["verdict"] == "PASS"
    assert doc["metrics"]["decode_frames"]["value"] == 4400
    assert doc["measurement_coverage"]["decode"]["count"] == 4400
    assert doc["metrics"]["decode_p95_ms"]["value"] == 72.91


def versions() -> None:
    for version in (None, "aa-projection-perf-capture/1"):
        raw = synthetic(20)
        raw["schema"] = version
        try:
            result(raw)
        except reporter.ContractError:
            continue
        raise AssertionError("historical/missing capture version promoted")
    doc = result(synthetic(20))
    doc.pop("measurement_coverage")
    doc["schema"] = "aa-projection-perf/1"
    historical = ROOT / "tools/perf/aa-projection-perf.schema.json"
    assert schema.validate(doc, schema.load_schema(historical)) == []
    assert schema.validate(doc, schema.load_schema(schema.DEFAULT_SCHEMA))


def no_window() -> None:
    raw = synthetic(20)
    raw.update(samples=[], active_projection_seconds=0)
    raw["video_evidence"] = {"blocks": [], "ipc_latency_ns": [], "decode_latency_ns": []}
    doc = result(raw)
    assert doc["verdict"] == "BLOCKED"
    parsed = coverage.parse_coverage(doc["measurement_coverage"])
    assert parsed.window is None
    assert parsed.sampling == coverage.Timeline(0, None, None, 0)
    assert "coverage:window" in doc["named_gaps"]


def capacity() -> None:
    item = coverage.CoverageLedger(0)
    for stamp in range(4096):
        assert item.add(coverage.Contribution("decode", 0, stamp, 0))
    assert item.add(coverage.Contribution("decode", 0, 4096, 0)) is False
    assert item.issues == ("coverage_capacity",)


def reporter_cli(raw_json: str) -> tuple[subprocess.CompletedProcess[str], str]:
    with tempfile.TemporaryDirectory(prefix="coverage-cli-") as directory:
        source, output = Path(directory) / "capture.json", Path(directory) / "result.json"
        source.write_text(raw_json, encoding="utf-8")
        process = subprocess.run([sys.executable, "-B", str(ROOT / "tools/perf/aa_perf_report.py"),
                                  "--capture", str(source), "--out", str(output), "--live"],
                                 capture_output=True, text=True, timeout=15, check=False)
        return process, output.read_text(encoding="utf-8") if output.exists() else ""


def latency_cli() -> None:
    for stream, ring in (("decode", "decode_latency_ns"), ("ipc_matches", "ipc_latency_ns")):
        # Given singleton endpoints whose persisted span cannot contain a four-second delta.
        raw = synthetic(1800)
        raw["video_evidence"][ring] = [4 * NS] * 360
        # When the actual reporter CLI processes the contradiction.
        process, output = reporter_cli(json.dumps(raw))
        print(stream, "impossible latency exit", process.returncode, process.stderr.strip())
        # Then the evidence is a contract error, not a false PASS.
        assert process.returncode == 4, output
        try:
            result(raw)
        except coverage.CoverageError:
            continue
        raise AssertionError("impossible latency did not raise CoverageError")


def ipc_subset_cli() -> None:
    # Given the exact Oracle contradiction, with aggregate counts and bytes unchanged.
    raw = synthetic(1800)
    blocks = raw["video_evidence"]["blocks"]
    blocks[0]["ipc_consumption"] = asdict(coverage.StreamBlock(0, None, None, 0, None, 0))
    blocks[1]["ipc_consumption"]["count"] = 2
    blocks[1]["ipc_consumption"]["bytes"] *= 2
    # When the actual reporter CLI sees matches before any consumed receive.
    process, output = reporter_cli(json.dumps(raw))
    print("IPC subset exit", process.returncode, process.stderr.strip())
    # Then the balanced totals cannot conceal the impossible per-block subset.
    assert process.returncode == 4, output


def high_latency_cli() -> None:
    # Given genuine four-second submission/completion spans, not a latency budget breach.
    raw = synthetic(1800)
    for block in raw["video_evidence"]["blocks"]:
        block["decode"]["begin_ns"] = block["decode"]["last_ns"] - 4 * NS
    raw["video_evidence"]["decode_latency_ns"] = [4 * NS] * 360
    # When the real CLI reports the consistent high-latency evidence.
    process, output = reporter_cli(json.dumps(raw))
    doc = json.loads(output)
    # Then four seconds remains informational, with no withdrawn 33ms gate.
    assert process.returncode == 0, process.stderr
    assert doc["verdict"] == "PASS" and doc["qualification_status"] == "COMPLETE_LIVE"
    assert doc["metrics"]["decode_p95_ms"]["value"] == 4000
    assert doc["metrics"]["decode_p95_ms"]["gated"] is False


def ipc_block_bounds() -> None:
    for matched_ns, consumed_ns in ((9 * NS, 10 * NS), (10 * NS, 9 * NS)):
        # Given valid aggregate bounds but a receive outside its block's consumed subset.
        raw = synthetic(20)
        block = raw["video_evidence"]["blocks"][1]
        block["ipc_consumption"].update(first_ns=consumed_ns, last_ns=consumed_ns)
        block["ipc_matches"].update(first_ns=matched_ns, last_ns=matched_ns,
                                    begin_ns=matched_ns - 1_000_000)
        # When / Then: reduction rejects both edges, not just aggregate count mismatches.
        rejects(raw)


def latency_capacity() -> None:
    # Given 4501 contributions but only one block contribution capable of four seconds.
    raw = synthetic(20)
    for block in raw["video_evidence"]["blocks"]:
        block["decode"]["count"] = 1500
    raw["video_evidence"]["blocks"][0]["decode"].update(count=1, begin_ns=NS)
    raw["video_evidence"]["decode_latency_ns"] = [4 * NS] + [40_000_000] * 4095
    assert result(raw)["measurement_coverage"]["decode"]["count"] == 4501
    raw["video_evidence"]["decode_latency_ns"][1] = 4 * NS
    # When / Then: a second impossible long delta cannot reuse that one contribution.
    rejects(raw)


CASES = {"valid": valid, "malformed": malformed, "missing": missing,
         "precedence": precedence, "ledger": ledger, "summary_mutations": summary_mutations,
         "edges": edges, "weighted_cpu": weighted_cpu, "retained": retained,
         "versions": versions, "no_window": no_window, "capacity": capacity,
         "latency_cli": latency_cli, "ipc_subset_cli": ipc_subset_cli,
         "high_latency_cli": high_latency_cli, "ipc_block_bounds": ipc_block_bounds,
         "latency_capacity": latency_capacity}

if __name__ == "__main__":
    names = sys.argv[1:] or list(CASES)
    for name in names:
        CASES[name]()
        print("PASS v2-coverage-" + name)
