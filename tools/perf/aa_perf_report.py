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
* validates the document it produced against its version's schema before
  writing it, so the tool can never emit an off-schema document.

Versions (#137): a capture ``aa-projection-perf-capture/2`` reports to an
``aa-projection-perf/2`` result under the historical budgets (``BUDGETS_V2``:
A/V max <= 50 ms), byte-for-byte the contract it was measured under, so history
is never reinterpreted. A capture ``/3`` (with ``av_series``) reports to an
``aa-projection-perf/3`` result under ``BUDGETS``: the signed A/V contract of
tools/perf/aa_av_contract.py over the replayed active window, and, live, the
moving-map workload.

Exit: 0 PASS, 1 FAIL (budget violation), 3 BLOCKED (prerequisite/metric
unavailable), 2 usage or I/O error, 4 internal contract violation.
"""
from __future__ import annotations

# allow: SIZE_OK -- Existing single-file result CLI contract; preserve lifecycle/budget semantics in this scoped policy change.

import argparse
import json
import math
import re
import sys
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aa_perf_schema  # noqa: E402
import aa_av_contract as av_contract  # noqa: E402
from aa_moving_map import MAX_DRAG_GAP_S, MIN_DRAG_RATE_HZ, cleanup_gaps, max_drag_gap_s  # noqa: E402
from aa_measurement_coverage import (
    CoverageError, NANO_PROFILE, Window, coverage_gaps, coverage_to_json, parse_capture, reduce_capture,
)
from aa_projection_collect import ProjectionProgress
from aa_build_guard import is_optimized

TASK = "32. Measure wired performance and two-hour soak"
SCHEMA_VERSION = "aa-projection-perf/3"
CAPTURE_SCHEMA = "aa-projection-perf-capture/3"
# Historical v2: replayed under its own contract, never promoted or relabelled.
SCHEMA_VERSION_V2 = "aa-projection-perf/2"
CAPTURE_SCHEMA_V2 = "aa-projection-perf-capture/2"
RESULT_FOR_CAPTURE = {CAPTURE_SCHEMA_V2: SCHEMA_VERSION_V2, CAPTURE_SCHEMA: SCHEMA_VERSION}
# Marker recorded on a run the build guard admitted only through its debugging
# override: the measured values stay visible, the run is never a PASS.
NOT_QUALIFYING_UNOPTIMIZED = "NOT_QUALIFYING: unoptimized_build"
EVENT_METRICS = ("frame_drops", "decode_p50_ms", "decode_p95_ms", "decode_fps",
                 "decode_frames", "ipc_throughput_mbps", "ipc_p95_added_latency_ms")

# Contractual budgets (plan "Contract budgets" line + task-32 acceptance).
# Do NOT edit to make a run pass.
# BUDGETS is the current (v3) contract: the signed A/V contract replaces the
# A/V max <= 50 ms budget (owner decision 2026-11-16; aa_av_contract.py). Its
# maximum is the soak rate rule (owner decision 2026-11-17): at most one pair
# over 90 ms per 1800 s of active window, none over 150 ms.
BUDGETS = {
    "receiver_rss_mb_max": 250.0,
    "receiver_cpu_pct_one_core_max": 50.0,
    "av_window_mean_abs_ms_max": av_contract.AV_MEAN_MAX_ABS_MS,
    "av_p99_abs_ms_max": av_contract.AV_P99_ABS_MAX_MS,
    "av_excursion_ms": av_contract.AV_SOAK_EXCURSION_MS,
    "av_excursions_per_s": av_contract.AV_SOAK_EXCURSIONS_PER_S,
    "av_hard_cap_ms": av_contract.AV_SOAK_HARD_CAP_MS,
    "av_coverage_pct_min": av_contract.AV_COVERAGE_MIN * 100,
    "audio_latency_ms_max": 200.0,
    "rss_growth_pct_max": 10.0,  # strict: growth must be < 10 %
    "crashes_max": 0,
    "helper_rss_mb_max": 32.0,
}
# The historical v2 contract, applied only to v2 captures (never edit).
BUDGETS_V2 = {
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


CATCH_UP_METRICS = ("audio_catch_up_events", "audio_catch_up_frames", "audio_catch_up_ms")


def _catch_up_metrics(events: dict) -> dict:
    """Informational catch-up loss (#154), never gated and never a stall dropout.

    The wired receiver drops a placement backlog past its 90 ms catch-up
    watermark in one run (``audio_catch_up``); the A/V metrics cannot see it
    (they measure from the placed slot), so the loss is reported on its own.
    A v3 capture recorded before the event existed carries none of the three
    keys and reports null; so does one whose collector could not establish
    the totals (a malformed record, or a media_telemetry_dropped notice after
    the newest catch-up record). A capture carrying only some keys, or values
    that are negative or disagree about whether any catch-up happened, is
    malformed.
    """
    present = [name in events for name in CATCH_UP_METRICS]
    if any(present) and not all(present):
        raise ContractError("capture.events must carry all or none of " + ", ".join(CATCH_UP_METRICS))
    count = _as_int(events.get("audio_catch_up_events"))
    frames = _as_int(events.get("audio_catch_up_frames"))
    duration = _as_float(events.get("audio_catch_up_ms"))
    values = (count, frames, duration)
    if any(value is not None and value < 0 for value in values):
        raise ContractError("catch-up counters must be nonnegative")
    if None not in values and len({value == 0 for value in values}) != 1:
        raise ContractError("catch-up events, frames and ms must agree on whether any catch-up happened")
    return {
        "audio_catch_up_events": _metric(
            count, "events",
            note="audio_catch_up events (#154): render steps that dropped a placement backlog past "
                 "the 90 ms catch-up watermark; not sink stalls, so never in audio_stall_*; null "
                 "when dropped media telemetry left the totals unconfirmed",
        ),
        "audio_catch_up_frames": _metric(
            frames, "frames",
            note="output-rate frames catch-up dropped beyond the ordinary bounded (5 ms per run) "
                 "drift trim; each run's ordinary drift-trim allowance is not included",
        ),
        "audio_catch_up_ms": _metric(
            duration, "ms",
            note="audio_catch_up_frames as audio duration at the logged output rate: audio the "
                 "phone sent that is never played, invisible to the A/V metrics",
        ),
    }


def _metric(
    value: float | None,
    unit: str,
    *,
    budget_max: float | None = None,
    budget_min: float | None = None,
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
            if op == "ge":
                failed = value < budget_min
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
    if budget_min is not None:
        entry["budget_min"] = budget_min
    if op != "le":
        entry["op"] = op
    if note is not None:
        entry["note"] = note
    return entry


def _load_logged_progress(capture: dict) -> dict[int, dict[str, dict]]:
    """Load persisted receiver-log evidence keyed by sample t_ns.

    Backward compatible with pre-#90 captures (no ``interference`` or
    ``progress_evidence``) and initial #90 captures (evidence only inside
    ``interference[*].logged_progress``).
    """
    raw_interference = capture.get("interference", [])
    if not isinstance(raw_interference, list) or any(not isinstance(item, dict)
                                                     for item in raw_interference):
        raise ContractError("capture.interference must be an array of objects")
    logged_by_ns: dict[int, dict[str, dict]] = {}
    for item in raw_interference:
        stamp = item.get("t_ns")
        logged = item.get("logged_progress")
        if type(stamp) is int and isinstance(logged, dict):
            logged_by_ns[stamp] = logged
    if "progress_evidence" in capture:
        raw_evidence = capture["progress_evidence"]
        if not isinstance(raw_evidence, list) or any(not isinstance(item, dict)
                                                     for item in raw_evidence):
            raise ContractError("capture.progress_evidence must be an array of objects")
        seen: set[int] = set()
        for item in raw_evidence:
            stamp = item.get("t_ns")
            logged = item.get("logged_progress")
            if (set(item) != {"t_ns", "logged_progress"}
                    or type(stamp) is not int or stamp < 0 or not isinstance(logged, dict)):
                raise ContractError(
                    "capture.progress_evidence entries must have nonnegative int t_ns and object logged_progress"
                )
            if stamp in seen:
                raise ContractError(f"duplicate t_ns in capture.progress_evidence: {stamp}")
            seen.add(stamp)
            logged_by_ns[stamp] = logged
    return logged_by_ns


def _build_record(capture: dict) -> dict:
    """The capture's build provenance, normalised for the result document.

    Older captures carry no ``build`` block: they report an unknown type and no
    build directory, never an assumed optimized build.
    """
    raw = capture.get("build")
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise ContractError("capture.build must be an object")
    build_type = raw.get("build_type")
    if build_type is not None and not isinstance(build_type, str):
        raise ContractError("capture.build.build_type must be a string or null")
    build_dir = raw.get("build_dir")
    if build_dir is not None and not isinstance(build_dir, str):
        raise ContractError("capture.build.build_dir must be a string or null")
    allow_unoptimized = raw.get("allow_unoptimized_build", False)
    if not isinstance(allow_unoptimized, bool):
        raise ContractError("capture.build.allow_unoptimized_build must be a boolean")
    verified_on = raw.get("verified_on") or ("target" if build_dir else "none")
    if verified_on not in ("target", "host", "none"):
        raise ContractError("capture.build.verified_on must be target, host or none")
    binaries = raw.get("binaries") or {}
    if not isinstance(binaries, dict):
        raise ContractError("capture.build.binaries must be an object")
    record = {
        "build_type": build_type,
        "build_dir": build_dir,
        "allow_unoptimized_build": allow_unoptimized,
        "verified_on": verified_on,
        "non_qualifying": None,
    }
    if binaries:
        record["binaries"] = binaries
    return record


def build_result(capture: dict, *, duration_requested: str | None, now: str,
                 capture_path: str | None, capture_sha256: str | None,
                 provenance_override: dict | None = None,
                 blocked_on_override: str | None = None) -> dict:
    if not isinstance(capture, dict):
        raise ContractError("capture must be a JSON object")
    if capture.get("schema") not in RESULT_FOR_CAPTURE:
        raise ContractError(f"unexpected capture schema: {capture.get('schema')!r}")
    version3 = capture["schema"] == CAPTURE_SCHEMA
    budgets = BUDGETS if version3 else BUDGETS_V2
    evidence = parse_capture(capture)
    capture, numeric_errors = aa_perf_schema.finite_numbers(capture)
    if capture.get("mode") != "wired":
        raise ContractError(f"capture mode must be 'wired', got {capture.get('mode')!r}")

    crashes = _as_int(capture.get("crashes"))
    if crashes is not None and crashes < 0:
        raise ContractError("crashes must be nonnegative")
    samples = evidence.samples
    progress = ProjectionProgress.replay(samples, _load_logged_progress(capture))
    window = (Window(progress.started_ns, progress.last_ns)
              if progress.started_ns is not None else None)
    reduced = reduce_capture(evidence, window)
    values = reduced.metrics
    duration_seconds = progress.seconds
    active_assertion = _as_float(capture.get("active_projection_seconds"))
    coverage_missing = list(coverage_gaps(reduced.measurement_coverage, active_assertion))

    events = capture.get("events") or {}
    if not isinstance(events, dict):
        raise ContractError("capture.events must be an object")
    if any(name in events for name in EVENT_METRICS if name != "frame_drops"):
        raise ContractError("v2 video metrics must be derived exclusively from video_evidence")
    helper_rss = events.get("helper_rss_mb")
    if helper_rss is not None:
        raise ContractError(
            "helper_rss_mb must be null on the wired path until todo 34 (plan-mandated)"
        )

    thermal_state = ({"peak_c": reduced.thermal_state.peak_c,
                      "zones": [{"zone": zone.zone, "temp_c": zone.temp_c}
                                for zone in reduced.thermal_state.zones]}
                     if reduced.thermal_state else None)

    metrics = {
        "receiver_rss_peak_mb": _metric(
            values["receiver_rss_peak_mb"], "MB", budget_max=budgets["receiver_rss_mb_max"], gated=True
        ),
        "receiver_rss_mean_mb": _metric(values["receiver_rss_mean_mb"], "MB"),
        "receiver_cpu_sustained_pct_one_core": _metric(
            values["receiver_cpu_sustained_pct_one_core"], "% of one core",
            budget_max=budgets["receiver_cpu_pct_one_core_max"], gated=True,
        ),
        "receiver_cpu_peak_pct_one_core": _metric(values["receiver_cpu_peak_pct_one_core"], "% of one core"),
        "rss_growth_pct": _metric(
            values["rss_growth_pct"], "%", budget_max=budgets["rss_growth_pct_max"], op="lt", gated=True
        ),
        "av_sync_ms": (_metric(
            _as_float(events.get("av_sync_ms")), "ms", budget_max=budgets["av_sync_ms_max"],
            gated=True,
        ) if not version3 else _metric(
            _as_float(events.get("av_sync_ms")), "ms",
            note="run-cumulative max |skew| through the measurement cutoff; v3 gates the "
                 "signed contract (av_window_mean_abs_ms, av_p99_abs_ms, av_excursions, av_hard_cap_pairs, "
                 "av_coverage_pct) instead")),
        "audio_latency_ms": _metric(
            _as_float(events.get("audio_latency_ms")), "ms",
            budget_max=budgets["audio_latency_ms_max"], gated=True,
        ),
        "audio_scheduling_skew_ms": _metric(
            _as_float(events.get("audio_scheduling_skew_ms")), "ms",
            note="audio write completion minus scheduled audio presentation; not A/V sync",
        ),
        "helper_rss_mb": _metric(None, "MB", budget_max=budgets["helper_rss_mb_max"],
                                 status="NOT_REQUIRED",
                                 note="wired path does not require the helper before todo 34"),
        "frame_drops": _metric(_as_int(events.get("frame_drops")), "frames"),
        "audio_stall_dropouts": _metric(
            _as_int(events.get("audio_stall_dropouts")), "events",
            note="audio sink stalls survived by dropping audio (#60); each is an audible gap",
        ),
        "audio_stall_dropped_chunks": _metric(
            _as_int(events.get("audio_stall_dropped_chunks")), "chunks",
            note="phone audio chunks dropped or partly cut while the sink was stalled, survived "
                 "or not; excludes in-flight chunks the blocked write fully delivered",
        ),
        "audio_stall_unrecovered": _metric(
            _as_int(events.get("audio_stall_unrecovered")), "events",
            note="sink stalls whose recovery failed, was stopped, or had not ended (#60)",
        ),
        "decode_p50_ms": _metric(values["decode_p50_ms"], "ms", note=DECODE_NOTE),
        "decode_p95_ms": _metric(values["decode_p95_ms"], "ms", note=DECODE_NOTE),
        "decode_fps": _metric(values["decode_fps"], "fps", note=DECODE_NOTE),
        "decode_frames": _metric(values["decode_frames"], "frames", note=DECODE_NOTE),
        "ipc_throughput_mbps": _metric(values["ipc_throughput_mbps"], "Mbps"),
        "ipc_p95_added_latency_ms": _metric(
            values["ipc_p95_added_latency_ms"], "ms"
        ),
        "thermal_peak_c": _metric(values["thermal_peak_c"], "C"),
    }

    if version3:
        metrics.update(_catch_up_metrics(events))

    av_block = None
    if version3:
        av_block, av_metrics = _signed_av(capture, window, invalid=(
            "av_sync_ms" in (capture.get("invalid_telemetry") or [])))
        metrics.update(av_metrics)

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
            bound = entry["budget_min"] if entry.get("op") == "ge" else entry["budget_max"]
            violations.append(
                {
                    "metric": name,
                    "value": entry["value"],
                    "budget_max": bound,
                    "detail": f"{entry['value']} {entry['unit']} violates budget "
                    f"{bound} ({entry.get('op', 'le')})",
                }
            )
    if crashes is None:
        unavailable.append("crashes")
    elif crashes > budgets["crashes_max"]:
        violations.append(
            {
                "metric": "crashes",
                "value": float(crashes),
                "budget_max": float(budgets["crashes_max"]),
                "detail": f"{crashes} crash(es); budget is {budgets['crashes_max']}",
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
    run_status.update(active_projection_seconds=progress.seconds if window else None,
                      startup_wait_seconds=progress.started_ns / 1e9
                      if progress.started_ns is not None else None,
                      projection_failure=progress.failure)
    if version3:
        # Build/compiler processes the collector saw on the target during the
        # stage (#137); null when the capture carries no watch at all.
        builds_seen = capture.get("concurrent_builds")
        run_status["concurrent_builds"] = (
            [{"t_s": item["t_ns"] / 1e9, "processes": [str(p) for p in item["processes"]]}
             for item in builds_seen]
            if isinstance(builds_seen, list) and all(
                isinstance(item, dict) and isinstance(item.get("t_ns"), int)
                and isinstance(item.get("processes"), list) for item in builds_seen)
            else None)
    if progress.interferences:
        # Shown beside the coverage result: a gap over 10 s is why coverage:sampling
        # fails. Omitted when empty so uninterrupted results are unchanged.
        run_status["interference"] = progress.interferences
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
    if requested_window is None or requested_window < 0:
        raise ContractError("requested duration must be finite and nonnegative")
    if progress.started_ns is not None:
        if requested_window is None or progress.seconds < requested_window:
            run_failures.append("incomplete_active_window")
        if progress.failure:
            run_failures.append(progress.failure)
    for failure in run_failures:
        violations.append({"metric": failure, "value": None, "budget_max": 0,
                           "detail": "projection run failed: " + failure})
    named_gaps = [name for name in (*EVENT_METRICS, "thermal_peak_c", "receiver_rss_peak_mb",
                                  "receiver_cpu_sustained_pct_one_core", "rss_growth_pct")
                  if metrics[name]["value"] is None] + coverage_missing
    unavailable.extend(named_gaps)
    invalid_telemetry = list(capture.get("invalid_telemetry") or []) + numeric_errors
    if invalid_telemetry:
        unavailable.append("invalid_telemetry")
    if origin == "live":
        if progress.started_ns is None:
            unavailable.append("run_evidence:active_projection_progress")
        for name in ("early_exit", "receiver_exit_code", "startup_ms", "video_units",
                     "audio_chunks", "display_survived", "display_connected"):
            value = run_status[name]
            if value is None or (name in ("video_units", "audio_chunks") and value <= 0):
                unavailable.append("run_evidence:" + name)
        if "observed_duration_seconds" not in capture:
            unavailable.append("run_evidence:observed_duration_seconds")
        if version3:
            # v3 soak stages qualify the moving-map workload (#137), and the
            # capture must prove it ran: stimulus started and enough completed
            # drags over the active window. Missing or insufficient evidence is
            # BLOCKED (the required workload was not measured), never a PASS;
            # a stimulus whose display control input stalled or closed is
            # already a FAIL (display_control_stalled / display_control_closed).
            gap = workload_gap(capture.get("workload"), window)
            if gap is not None:
                unavailable.append("run_evidence:" + gap)
            # The phone restore after the stage must be shown complete (media
            # player stopped and verified, USB restored): a stage that leaves the
            # player running is never a PASS, and a non-PASS stops the chain
            # before the next stage. Named BLOCKED reasons (cleanup:*).
            unavailable.extend(cleanup_gaps(capture.get("workload")))
            # A build on the target during the stage contends for CPU and memory:
            # the stage measured something else than the quiet host it claims.
            # Interference, not a product FAIL: it BLOCKS a stage that would
            # otherwise pass (a measured budget miss still FAILs, as for every
            # BLOCKED reason). A capture without the watch is not proven quiet.
            if run_status["concurrent_builds"] is None:
                unavailable.append("run_evidence:concurrent_build_watch")
            elif run_status["concurrent_builds"]:
                unavailable.append("interference:concurrent_build")

    provenance_source = provenance_override if provenance_override is not None else capture.get("provenance")
    provenance_source, provenance_numeric_errors = aa_perf_schema.finite_numbers(provenance_source)
    invalid_telemetry.extend(provenance_numeric_errors)
    provenance = _normalise_provenance(provenance_source)
    if not provenance["target_hardware_state"] and "target_hardware_state" in capture:
        provenance["target_hardware_state"] = capture["target_hardware_state"]
    if evidence.thermal_policy is not None and provenance["target_hardware_state"].get("model") != evidence.thermal_policy.model:
        raise ContractError("provenance hardware does not match approved Nano thermal policy")
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

    # A run the build guard admitted only through its debugging override measured
    # something other than the qualified build, so it is never a PASS: the values
    # stay visible, the qualification verdict does not.
    build = _build_record(capture)
    unoptimized_run = (build["allow_unoptimized_build"]
                       or (build["build_type"] is not None
                           and not is_optimized(build["build_type"])))
    if unoptimized_run:
        build["non_qualifying"] = NOT_QUALIFYING_UNOPTIMIZED
        if verdict == "PASS":
            verdict = "BLOCKED"
            blocked_on = NOT_QUALIFYING_UNOPTIMIZED

    notes = [
        "decode timing is recorded informationally; the p95 <=33 ms budget is WITHDRAWN "
        "by task-9 owner sign-off and is NOT asserted by this tool",
        "helper RSS is null: the wired path does not require the helper before todo 34",
        "provenance equivalence is provisional; post-package equivalence is todo 43",
    ]
    for gap in progress.interferences:
        notes.append(
            f"collector sampling gap of {gap['gap_seconds']:.1f} s ending at t={gap['t_ns'] / 1e9:.1f} s "
            "is host interference, not a receiver verdict; gaps over 10 s fail coverage:sampling, "
            "see run_status.interference")
    if not samples:
        notes.append("capture contained no samples; gated metrics are unavailable")
    if unoptimized_run:
        notes.append(
            f"{NOT_QUALIFYING_UNOPTIMIZED}: the build guard was overridden, so the measured "
            "values are informational and this run is not a qualification result; "
            "re-run against an optimized build")
    if evidence.thermal_policy is not None:
        notes.append("Nano-specific owner-approved CV availability exception; not full-all-zone thermal qualification. "
                     "CV nulls mean unavailable, not measured temperatures; cause unproven. "
                     "Continuous CPU/GPU/SoC0/SoC1/SoC2/TJ and stable inventory remain required.")

    result = {
        "schema": RESULT_FOR_CAPTURE[capture["schema"]],
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
        "budgets": dict(budgets),
        "metrics": metrics,
        "measurement_coverage": coverage_to_json(reduced.measurement_coverage),
        "thermal_state": thermal_state,
        "violations": violations,
        "provenance": provenance,
        "notes": notes,
        "build": build,
    }
    if evidence.thermal_policy is not None:
        result["thermal_policy"] = asdict(evidence.thermal_policy)
    if version3:
        result["av_contract"] = av_block
        result["workload"] = _workload_record(capture.get("workload"), window)
    if capture_path is not None:
        result["capture"] = {"path": capture_path, "sha256": capture_sha256}

    # Never emit an off-schema document: validate before returning.
    errors = aa_perf_schema.validate(result, aa_perf_schema.load_schema(
        aa_perf_schema.schema_for(result["schema"])))
    if errors:
        raise ContractError("internal: produced document is off-schema: " + "; ".join(errors))
    return result


def _signed_av(capture: dict, window: Window | None, *, invalid: bool) -> tuple[dict, dict]:
    """The v3 signed A/V contract over the replayed active window.

    Pairs and audio intervals come from the capture's ``av_series`` (ns relative
    to the monotonic origin, frozen at the measurement cutoff). Invalid A/V
    telemetry, a missing series or no active window leave every A/V metric
    unavailable (BLOCKED, as v2); measured coverage below the floor, including
    no audio at all, is a FAIL.
    """
    series = capture.get("av_series")
    stats = None
    if not invalid and window is not None and isinstance(series, dict):
        times, skews = series.get("pair_t_ns"), series.get("pair_skew_ns")
        intervals = series.get("audio_intervals_ns")
        integer_list = lambda v: isinstance(v, list) and all(  # noqa: E731
            isinstance(x, int) and not isinstance(x, bool) for x in v)
        if (integer_list(times) and integer_list(skews) and len(times) == len(skews)
                and isinstance(intervals, list)
                and all(isinstance(i, list) and len(i) == 2 and integer_list(i) for i in intervals)):
            pairs = [(t, skew / 1e6) for t, skew in zip(times, skews)]
            stats = av_contract.evaluate(pairs, [tuple(i) for i in intervals],
                                         window.start_ns, window.end_ns, max_clause="soak_rate")
        else:
            raise ContractError("capture.av_series must hold integer pair_t_ns/pair_skew_ns of equal "
                                "length and [start, end] audio_intervals_ns")
    value = (lambda key: stats[key] if stats is not None else None)
    worst = value("worst_window_mean_ms")
    coverage = value("coverage")
    metrics = {
        "av_window_mean_abs_ms": _metric(
            abs(worst) if worst is not None else None, "ms",
            budget_max=BUDGETS["av_window_mean_abs_ms_max"], gated=True,
            note="largest |mean signed skew| over every full 5 s window of the active window"),
        "av_p99_abs_ms": _metric(value("abs_p99_ms"), "ms", budget_max=BUDGETS["av_p99_abs_ms_max"],
                                 gated=True, note="nearest-rank p99 of |signed skew|"),
        "av_max_abs_ms": _metric(value("abs_max_ms"), "ms",
                                 note="max |signed skew| in the active window; informational "
                                      "(the soak judges the excursion rate and the hard cap)"),
        "av_excursions": _metric(
            value("excursions"), "pairs", budget_max=value("excursion_allowance"), gated=True,
            note=f"pairs with |signed skew| > {BUDGETS['av_excursion_ms']:g} ms; allowance "
                 f"floor(active s / {av_contract.AV_SOAK_EXCURSION_PERIOD_S})"),
        "av_hard_cap_pairs": _metric(
            value("over_hard_cap"), "pairs", budget_max=0, gated=True,
            note=f"pairs with |signed skew| > {BUDGETS['av_hard_cap_ms']:g} ms (hard cap)"),
        "av_coverage_pct": _metric(
            coverage * 100 if coverage is not None else None, "%",
            budget_min=BUDGETS["av_coverage_pct_min"], op="ge", gated=True,
            note="valid audio slots' scheduled union over the active window"),
    }
    block = {"limits": dict(av_contract.SOAK_LIMITS), "valid": stats is not None,
             **({key: stats[key] for key in stats} if stats is not None else {"passed": None})}
    return block, metrics


def workload_gap(raw: object, window: Window | None) -> str | None:
    """Why a v3 workload record does not prove the moving map ran, or None.

    In order: no moving-map record; the stimulus never started; a required
    phone command failed (the harness lost the phone: BLOCKED, not FAIL); fewer
    than MIN_DRAG_RATE_HZ drags per active second; a stretch of the active
    window longer than MAX_DRAG_GAP_S without a drag (or no drag evidence); a
    malformed media relaunch count.
    """
    if not isinstance(raw, dict) or raw.get("kind") != "moving-map":
        return "moving_map_workload"
    drags = raw.get("drags")
    if raw.get("stimulus_started") is not True or not isinstance(drags, int) or isinstance(drags, bool):
        return "moving_map_stimulus"
    if "phone_failure" not in raw or raw["phone_failure"] is not None:
        return "moving_map_phone"
    if window is None or drags < MIN_DRAG_RATE_HZ * (window.end_ns - window.start_ns) / 1e9:
        return "moving_map_drags"
    gap = _drag_gap(raw, window)
    if gap is None or gap > MAX_DRAG_GAP_S:
        return "moving_map_drag_gap"
    relaunches = raw.get("media_relaunches")
    if not isinstance(relaunches, int) or isinstance(relaunches, bool) or relaunches < 0:
        return "moving_map_media"
    return None


def _drag_gap(raw: dict, window: Window | None) -> float | None:
    if window is None:
        return None
    return max_drag_gap_s(raw.get("drag_t_ns"), window.start_ns, window.end_ns)


def _workload_record(raw: object, window: Window | None) -> dict:
    """The result's workload record. For moving-map every evidence field is present;
    a missing or malformed one is recorded as not proven (0 drags, not started,
    an unknown phone failure, no drag gap), and ``workload_gap`` then keeps the
    stage from qualifying."""
    if not isinstance(raw, dict) or raw.get("kind") != "moving-map":
        return {"kind": "none"}
    count = lambda key: (raw[key] if isinstance(raw.get(key), int)  # noqa: E731
                         and not isinstance(raw.get(key), bool) and raw[key] >= 0 else 0)
    failure = raw.get("phone_failure", "not recorded")
    # The phone restore after the stage (media stopped and verified, then USB
    # mode): recorded, and a failure BLOCKS a live stage (cleanup_gaps).
    cleanup = raw.get("cleanup_failures")
    cleanup = ([str(item)[:300] for item in cleanup] if isinstance(cleanup, list)
               else ["cleanup not recorded"])
    return {"kind": "moving-map", "media_relaunches": count("media_relaunches"),
            "drags": count("drags"), "stimulus_started": raw.get("stimulus_started") is True,
            "phone_failure": failure if failure is None else str(failure)[:300],
            "max_drag_gap_s": _drag_gap(raw, window), "cleanup_failures": cleanup}


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
    parser.add_argument("--thermal-policy", choices=("strict-all-zones", NANO_PROFILE), default=None,
                        help="require the capture's policy selection; never relabel a capture")
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
        if args.thermal_policy is not None:
            evidence = parse_capture(capture)
            selected = evidence.thermal_policy.profile if evidence.thermal_policy is not None else "strict-all-zones"
            if args.thermal_policy != selected:
                raise ContractError("requested thermal policy does not match capture; replay cannot opt in")
        result = build_result(
            capture,
            duration_requested=args.duration_requested,
            now=now,
            capture_path=str(args.capture),
            capture_sha256=args.capture_sha256,
            provenance_override=provenance_override,
            blocked_on_override=args.blocked_on,
        )
    except (ContractError, CoverageError) as exc:
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
