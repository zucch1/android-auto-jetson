#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Task-32 perf reporter: turn a raw capture into a validated result document.

Stdlib only. The capture (produced live by ``tools/perf/aa-projection-collect.py``
on the target, or supplied by a fixture) holds time-series samples plus
event-derived observations. This reporter:

* computes every observable metric (receiver RSS peak/mean, sustained and peak
  CPU, RSS growth, A/V sync, audio latency, frame drops, decode and IPC timing,
  thermal state),
* compares each *gated* metric against the CONTRACTUAL budgets (fixed here and
  in the schema; never weakened, never hidden),
* fails closed: an unavailable gated metric yields BLOCKED, a measured budget
  miss yields FAIL and a non-zero exit,
* enforces the plan-mandated provenance contract: ``package_digest`` is forced
  to ``null`` and ``equivalence_status`` to ``"provisional"``; a capture that
  claims otherwise is rejected,
* records ``helper_rss_mb`` as ``null`` (the wired path does not require the
  helper before todo 34); a non-null value is rejected,
* validates the document it produced against ``aa-projection-perf.schema.json``
  before writing it, so the tool can never emit an off-schema document.

Exit: 0 PASS, 1 FAIL (budget violation), 3 BLOCKED (prerequisite/metric
unavailable), 2 usage or I/O error, 4 internal contract violation.
"""
from __future__ import annotations

import argparse
import json
import math
import re
import sys
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aa_perf_schema  # noqa: E402

TASK = "32. Measure wired performance and two-hour soak"
SCHEMA_VERSION = "aa-projection-perf/1"
CAPTURE_SCHEMA = "aa-projection-perf-capture/1"
EVENT_METRICS = ("frame_drops", "decode_p50_ms", "decode_p95_ms", "decode_fps",
                 "decode_frames", "ipc_throughput_mbps", "ipc_p95_added_latency_ms")

# Contractual budgets (plan "Contract budgets" line + task-32 acceptance).
# Do NOT edit to make a run pass.
BUDGETS = {
    "receiver_rss_mb_max": 250.0,
    "receiver_cpu_pct_one_core_max": 50.0,
    "av_sync_ms_max": 50.0,
    "audio_latency_ms_max": 200.0,
    "rss_growth_pct_max": 10.0,  # strict: growth must be < 10 %
    "crashes_max": 0,
    "helper_rss_mb_max": 32.0,
}

# Rewrite note carried on the decode metrics: the <=33 ms p95 budget is
# WITHDRAWN (task-9 owner sign-off). The tool measures decode timing but never
# gates it and never asserts the withdrawn budget.
DECODE_NOTE = "informational only; p95 <=33 ms budget WITHDRAWN by task-9 owner sign-off"


class ContractError(RuntimeError):
    """The capture violates a plan-mandated contract; fail closed (exit 4)."""


def _as_float(value: object) -> float | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ContractError(f"expected a number or null, got {value!r}")
    try:
        number = float(value)
    except OverflowError:
        return None
    return number if math.isfinite(number) else None


def _as_int(value: object) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ContractError(f"expected an integer or null, got {value!r}")
    return value


def _metric(
    value: float | None,
    unit: str,
    *,
    budget_max: float | None = None,
    op: str = "le",
    gated: bool = False,
    status: str | None = None,
    note: str | None = None,
) -> dict:
    if value is not None and not math.isfinite(value):
        value = None
    if status is None:
        if not gated:
            status = "INFORMATIONAL"
        elif value is None:
            status = "UNAVAILABLE"
        else:
            failed = value > budget_max if op == "le" else value >= budget_max
            status = "FAIL" if failed else "PASS"
    entry: dict = {
        "value": value,
        "unit": unit,
        "budget_max": budget_max,
        "gated": gated,
        "status": status,
    }
    if op != "le":
        entry["op"] = op
    if note is not None:
        entry["note"] = note
    return entry


def _mean(values: list[float]) -> float | None:
    return sum(values) / len(values) if values else None


def _peak(values: list[float]) -> float | None:
    return max(values) if values else None


def _thermal(capture: dict, samples: list[dict]) -> dict | None:
    peaks: list[float] = []
    zones: list[dict] = []
    for sample in samples:
        sample_peak = _as_float(sample.get("thermal_peak_c"))
        if sample_peak is not None:
            peaks.append(sample_peak)
        for zone in sample.get("thermal_zones") or []:
            if not isinstance(zone, dict) or "temp_c" not in zone:
                continue
            temp = _as_float(zone.get("temp_c"))
            if temp is None:
                continue
            peaks.append(temp)
            if not zones:
                zones.append({"zone": str(zone.get("zone", "unknown")), "temp_c": temp})
    if not peaks and not zones:
        return None
    return {"peak_c": _peak(peaks), "zones": zones}


def build_result(capture: dict, *, duration_requested: str | None, now: str,
                 capture_path: str | None, capture_sha256: str | None,
                 provenance_override: dict | None = None,
                 blocked_on_override: str | None = None) -> dict:
    if not isinstance(capture, dict):
        raise ContractError("capture must be a JSON object")
    capture, numeric_errors = aa_perf_schema.finite_numbers(capture)
    if capture.get("schema") not in (CAPTURE_SCHEMA, None):
        raise ContractError(f"unexpected capture schema: {capture.get('schema')!r}")
    if capture.get("mode") != "wired":
        raise ContractError(f"capture mode must be 'wired', got {capture.get('mode')!r}")

    duration_seconds = _as_float(capture.get("observed_duration_seconds",
                                             capture.get("duration_seconds")))
    if duration_seconds is None or duration_seconds < 0:
        raise ContractError("capture duration must be a finite non-negative number")
    crashes = _as_int(capture.get("crashes"))

    samples = capture.get("samples") or []
    if not isinstance(samples, list):
        raise ContractError("capture.samples must be an array")
    rss_series = [v for s in samples if (v := _as_float(s.get("receiver_rss_mb"))) is not None]
    cpu_series = [
        v for s in samples if (v := _as_float(s.get("receiver_cpu_pct_one_core"))) is not None
    ]
    rss_growth: float | None = None
    if len(rss_series) >= 2 and rss_series[0] > 0:
        rss_growth = (rss_series[-1] - rss_series[0]) / rss_series[0] * 100.0

    events = capture.get("events") or {}
    if not isinstance(events, dict):
        raise ContractError("capture.events must be an object")
    helper_rss = events.get("helper_rss_mb")
    if helper_rss is not None:
        raise ContractError(
            "helper_rss_mb must be null on the wired path until todo 34 (plan-mandated)"
        )

    thermal_state = _thermal(capture, samples)
    thermal_peak = thermal_state["peak_c"] if thermal_state else None

    metrics = {
        "receiver_rss_peak_mb": _metric(
            _peak(rss_series), "MB", budget_max=BUDGETS["receiver_rss_mb_max"], gated=True
        ),
        "receiver_rss_mean_mb": _metric(_mean(rss_series), "MB"),
        "receiver_cpu_sustained_pct_one_core": _metric(
            _mean(cpu_series), "% of one core",
            budget_max=BUDGETS["receiver_cpu_pct_one_core_max"], gated=True,
        ),
        "receiver_cpu_peak_pct_one_core": _metric(_peak(cpu_series), "% of one core"),
        "rss_growth_pct": _metric(
            rss_growth, "%", budget_max=BUDGETS["rss_growth_pct_max"], op="lt", gated=True
        ),
        "av_sync_ms": _metric(
            _as_float(events.get("av_sync_ms")), "ms", budget_max=BUDGETS["av_sync_ms_max"],
            gated=True,
        ),
        "audio_latency_ms": _metric(
            _as_float(events.get("audio_latency_ms")), "ms",
            budget_max=BUDGETS["audio_latency_ms_max"], gated=True,
        ),
        "audio_scheduling_skew_ms": _metric(
            _as_float(events.get("audio_scheduling_skew_ms")), "ms",
            note="audio write completion minus scheduled audio presentation; not A/V sync",
        ),
        "helper_rss_mb": _metric(None, "MB", budget_max=BUDGETS["helper_rss_mb_max"],
                                 status="NOT_REQUIRED",
                                 note="wired path does not require the helper before todo 34"),
        "frame_drops": _metric(_as_int(events.get("frame_drops")), "frames"),
        "decode_p50_ms": _metric(_as_float(events.get("decode_p50_ms")), "ms", note=DECODE_NOTE),
        "decode_p95_ms": _metric(_as_float(events.get("decode_p95_ms")), "ms", note=DECODE_NOTE),
        "decode_fps": _metric(_as_float(events.get("decode_fps")), "fps", note=DECODE_NOTE),
        "decode_frames": _metric(_as_int(events.get("decode_frames")), "frames", note=DECODE_NOTE),
        "ipc_throughput_mbps": _metric(_as_float(events.get("ipc_throughput_mbps")), "Mbps"),
        "ipc_p95_added_latency_ms": _metric(
            _as_float(events.get("ipc_p95_added_latency_ms")), "ms"
        ),
        "thermal_peak_c": _metric(thermal_peak, "C"),
    }

    # Violations + verdict. A measured violation is a definite FAIL; an
    # unavailable gated metric is BLOCKED (never a pass).
    violations: list[dict] = []
    unavailable: list[str] = []
    for name, entry in metrics.items():
        if not entry["gated"]:
            continue
        if entry["value"] is None:
            unavailable.append(name)
        elif entry["status"] == "FAIL":
            violations.append(
                {
                    "metric": name,
                    "value": entry["value"],
                    "budget_max": entry["budget_max"],
                    "detail": f"{entry['value']} {entry['unit']} violates budget "
                    f"{entry['budget_max']} ({entry.get('op', 'le')})",
                }
            )
    if crashes is None:
        unavailable.append("crashes")
    elif crashes > BUDGETS["crashes_max"]:
        violations.append(
            {
                "metric": "crashes",
                "value": float(crashes),
                "budget_max": float(BUDGETS["crashes_max"]),
                "detail": f"{crashes} crash(es); budget is {BUDGETS['crashes_max']}",
            }
        )

    origin = capture.get("measurement_origin", "offline")
    if origin not in ("live", "offline"):
        raise ContractError("measurement_origin must be live or offline")
    run_status = {name: capture.get(name) for name in
                  ("early_exit", "receiver_exit_code", "receiver_stopped_by_collector",
                   "display_survived", "display_connected", "terminal_errors")}
    run_status.update({name: events.get(name) for name in
                       ("startup_ms", "video_units", "audio_chunks")})
    run_failures = []
    if run_status["early_exit"]:
        run_failures.append("early_exit")
    if run_status["receiver_exit_code"] not in (None, 0):
        run_failures.append("receiver_exit_code")
    if run_status["display_survived"] is False:
        run_failures.append("display_survived")
    if run_status["display_connected"] is False:
        run_failures.append("display_disconnected")
    if run_status["terminal_errors"]:
        run_failures.append("terminal_errors")
    requested_window = _as_float(capture.get("duration_seconds"))
    if requested_window is not None and duration_seconds < requested_window:
        run_failures.append("incomplete_window")
    for failure in run_failures:
        violations.append({"metric": failure, "value": None, "budget_max": 0,
                           "detail": "projection run failed: " + failure})
    named_gaps = [name for name in EVENT_METRICS if metrics[name]["value"] is None]
    unavailable.extend(named_gaps)
    invalid_telemetry = list(capture.get("invalid_telemetry") or []) + numeric_errors
    if invalid_telemetry:
        unavailable.append("invalid_telemetry")
    if origin == "live":
        for name in ("early_exit", "receiver_exit_code", "startup_ms", "video_units",
                     "audio_chunks", "display_survived", "display_connected"):
            value = run_status[name]
            if value is None or (name in ("video_units", "audio_chunks") and value <= 0):
                unavailable.append("run_evidence:" + name)
        if "observed_duration_seconds" not in capture:
            unavailable.append("run_evidence:observed_duration_seconds")

    provenance_source = provenance_override if provenance_override is not None else capture.get("provenance")
    provenance_source, provenance_numeric_errors = aa_perf_schema.finite_numbers(provenance_source)
    invalid_telemetry.extend(provenance_numeric_errors)
    provenance = _normalise_provenance(provenance_source)
    if not provenance["target_hardware_state"] and "target_hardware_state" in capture:
        provenance["target_hardware_state"] = capture["target_hardware_state"]
    provenance_gaps = []
    if origin == "live":
        for name in ("source_digest", "build_artifact_digest", "configuration_digest"):
            if not _digest(provenance[name]):
                provenance_gaps.append(name)
        services = provenance["service_definitions"]
        if not services or any(not s.get("name") or not _digest(s.get("digest")) for s in services):
            provenance_gaps.append("service_definitions")
        deps = provenance["dependency_versions"]
        if not deps or any(not name or not version for name, version in deps.items()):
            provenance_gaps.append("dependency_versions")
    unavailable.extend("provenance:" + name for name in provenance_gaps)
    if provenance_numeric_errors:
        unavailable.append("invalid_telemetry")

    blocked_on: str | None = None
    if violations:
        verdict = "FAIL"
    elif unavailable:
        verdict = "BLOCKED"
        blocked_on = "unavailable_metric:" + ",".join(sorted(unavailable))
    else:
        verdict = "PASS"

    if blocked_on_override is not None:
        # A forced BLOCKED verdict must never hide a measured budget miss
        # (misleading_success_output): only a capture without violations may
        # be reported as BLOCKED.
        if violations:
            raise ContractError(
                "cannot force BLOCKED over measured budget violations: "
                + ", ".join(v["metric"] for v in violations)
            )
        verdict = "BLOCKED"
        blocked_on = blocked_on_override

    notes = [
        "decode timing is recorded informationally; the p95 <=33 ms budget is WITHDRAWN "
        "by task-9 owner sign-off and is NOT asserted by this tool",
        "helper RSS is null: the wired path does not require the helper before todo 34",
        "provenance equivalence is provisional; post-package equivalence is todo 43",
    ]
    if not samples:
        notes.append("capture contained no samples; gated metrics are unavailable")

    result = {
        "schema": SCHEMA_VERSION,
        "task": TASK,
        "generated_at_utc": now,
        "mode": "wired",
        "duration_requested": duration_requested,
        "duration_seconds": duration_seconds,
        "measurement_origin": origin,
        "qualification_status": ("INCOMPLETE" if verdict != "PASS" else
                                 "COMPLETE_LIVE" if origin == "live" else "OFFLINE_ONLY"),
        "named_gaps": named_gaps,
        "provenance_gaps": provenance_gaps,
        "invalid_telemetry": invalid_telemetry,
        "run_status": run_status,
        "verdict": verdict,
        "blocked_on": blocked_on,
        "crashes": crashes if crashes is not None else 0,
        "budgets": dict(BUDGETS),
        "metrics": metrics,
        "thermal_state": thermal_state,
        "violations": violations,
        "provenance": provenance,
        "notes": notes,
    }
    if capture_path is not None:
        result["capture"] = {"path": capture_path, "sha256": capture_sha256}

    # Never emit an off-schema document: validate before returning.
    errors = aa_perf_schema.validate(result, aa_perf_schema.load_schema(aa_perf_schema.DEFAULT_SCHEMA))
    if errors:
        raise ContractError("internal: produced document is off-schema: " + "; ".join(errors))
    return result


def _digest(value: str | None) -> bool:
    return isinstance(value, str) and re.fullmatch(r"(?:sha256:)?[0-9a-f]{64}", value) is not None


def _normalise_provenance(raw: object) -> dict:
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise ContractError("capture.provenance must be an object")
    if raw.get("package_digest") is not None:
        raise ContractError(
            "capture.provenance.package_digest must be null (equivalence is todo 43)"
        )
    if raw.get("equivalence_status") not in (None, "provisional"):
        raise ContractError(
            "capture.provenance.equivalence_status must be 'provisional' (todo 43 owns equivalence)"
        )
    service_definitions = raw.get("service_definitions") or []
    if not isinstance(service_definitions, list):
        raise ContractError("capture.provenance.service_definitions must be an array")
    dependency_versions = raw.get("dependency_versions") or {}
    if not isinstance(dependency_versions, dict):
        raise ContractError("capture.provenance.dependency_versions must be an object")
    target_state = raw.get("target_hardware_state") or {}
    if not isinstance(target_state, dict):
        raise ContractError("capture.provenance.target_hardware_state must be an object")
    return {
        "source_digest": raw.get("source_digest"),
        "build_artifact_digest": raw.get("build_artifact_digest"),
        "configuration_digest": raw.get("configuration_digest"),
        "service_definitions": service_definitions,
        "dependency_versions": dependency_versions,
        "target_hardware_state": target_state,
        "package_digest": None,
        "equivalence_status": "provisional",
    }


def _cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--duration-requested", default=None)
    parser.add_argument("--capture-sha256", default=None)
    parser.add_argument("--provenance", type=Path, default=None,
                        help="JSON provenance block computed by the host wrapper")
    parser.add_argument("--now", default=None, help="override generated_at_utc (determinism/tests)")
    parser.add_argument("--blocked-on", default=None,
                         help="force a BLOCKED verdict with this reason (pre-run prerequisites)")
    parser.add_argument("--live", action="store_true")
    args = parser.parse_args(argv)

    provenance_override: dict | None = None
    if args.provenance is not None:
        try:
            provenance_override = json.loads(args.provenance.read_text(encoding="utf-8"))
        except OSError as exc:
            print(f"error: cannot read provenance {args.provenance}: {exc}", file=sys.stderr)
            return 2
        except json.JSONDecodeError as exc:
            print(f"error: provenance is not valid JSON: {exc}", file=sys.stderr)
            return 2
        if not isinstance(provenance_override, dict):
            print("error: provenance must be a JSON object", file=sys.stderr)
            return 2

    try:
        capture = json.loads(args.capture.read_text(encoding="utf-8"))
    except OSError as exc:
        print(f"error: cannot read capture {args.capture}: {exc}", file=sys.stderr)
        return 2
    except json.JSONDecodeError as exc:
        print(f"error: capture is not valid JSON: {exc}", file=sys.stderr)
        return 2

    now = args.now or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    if args.live and isinstance(capture, dict):
        capture["measurement_origin"] = "live"
    try:
        result = build_result(
            capture,
            duration_requested=args.duration_requested,
            now=now,
            capture_path=str(args.capture),
            capture_sha256=args.capture_sha256,
            provenance_override=provenance_override,
            blocked_on_override=args.blocked_on,
        )
    except ContractError as exc:
        print(f"contract violation: {exc}", file=sys.stderr)
        return 4

    try:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2, sort_keys=True, allow_nan=False) + "\n", encoding="utf-8")
    except OSError as exc:
        print(f"error: cannot write {args.out}: {exc}", file=sys.stderr)
        return 2

    verdict = result["verdict"]
    summary = ", ".join(
        f"{name}={'null' if m['value'] is None else m['value']}"
        for name, m in sorted(result["metrics"].items())
        if m["gated"]
    )
    print(f"verdict={verdict} crashes={result['crashes']} gated: {summary}")
    if result["violations"]:
        for v in result["violations"]:
            print(f"  violation: {v['detail']}", file=sys.stderr)
    if result["blocked_on"]:
        print(f"  blocked_on: {result['blocked_on']}", file=sys.stderr)
    return {"PASS": 0, "FAIL": 1, "BLOCKED": 3}[verdict]


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
