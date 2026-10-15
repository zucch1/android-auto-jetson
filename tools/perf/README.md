# Wired projection performance + soak measurement (task 32)

`aa-projection-perf.sh` measures wired (USB/AOA) Android Auto projection
performance on the qualified Jetson and evaluates the **contractual** budgets.
It is the task-32 deliverable: phase A ships the tool, its JSON schema, its
fixtures/tests and the phase-B runbook; the actual 30-minute performance run and
2-hour soak are run later under orchestrator control (see
`docs/qualification/wired-perf-runbook.md`).

```
tools/perf/
  aa-projection-perf.sh            entry point (strict CLI, fail-closed)
  aa_projection_collect.py         on-target collector (launched over ssh)
  aa_perf_report.py                capture -> result document + budget verdict
  aa_perf_schema.py                stdlib JSON Schema validator (own output)
  aa-projection-perf.schema.json   the result-document contract
  README.md                        this file
```

## Contractual budgets

Fixed in `aa-projection-perf.schema.json` and `aa_perf_report.py`. They are
never weakened and a diagnostics-suppressing flag does not exist:

| Metric | Budget | Gate |
| --- | --- | --- |
| receiver RSS | <= 250 MB (peak) | FAIL on miss |
| receiver CPU | <= 50 % of one core (sustained) | FAIL on miss |
| RSS growth over the run | < 10 % | FAIL on miss |
| A/V sync | <= 50 ms | FAIL on miss |
| audio path latency | <= 200 ms | FAIL on miss |
| crashes | 0 | FAIL on any |
| helper RSS | `null` | recorded `null` (wired path needs no helper before todo 34) |

Frame drops, decode timing (p50/p95/fps/frames), IPC throughput/latency, mean
RSS and peak CPU and the thermal state are **recorded** but are not gates. The
decode p95 <= 33 ms budget is **WITHDRAWN** (task-9 owner sign-off): decode
timing is reported informationally and the tool never asserts it.

Exit codes: `0` PASS, `1` FAIL (budget violation), `2` usage error, `3` BLOCKED
(prerequisite or gated metric unavailable), `4` internal contract violation.
A green run without measured values is never a pass: if a gated metric cannot be
measured the run is BLOCKED.

## Usage

```sh
tools/perf/aa-projection-perf.sh --mode wired --duration 30m --out /tmp/perf-30m.json
tools/perf/aa-projection-perf.sh --mode wired --duration 2h  --out /tmp/soak-2h.json
tools/perf/aa-projection-perf.sh --readiness-only --out /tmp/readiness.json
tools/perf/aa-projection-perf.sh --mode wired --duration 30m \
    --capture capture.json --out result.json     # offline: report a capture
```

`--duration` is strict: `<int>[smh]?` (no unit = seconds). Anything else is a
usage error (exit 2). `--mode` must be `wired`. `--host` (default
`jetson.local`) and `--port` (default `1-2.1`) select the target; `--receiver`
and `--display` name the installed binaries. Every ssh is wrapped in `timeout`.

## Measurement sources

The collector runs on the target and reads only procfs/sysfs plus the receiver's
newline-delimited JSON event stream:

| Metric | Source |
| --- | --- |
| receiver RSS / CPU | `/proc/<pid>/status` (`VmRSS`), `/proc/<pid>/stat` (utime+stime over the sampling interval) |
| thermal state | `/sys/class/thermal/thermal_zone*/{temp,type}` |
| frame drops | `diag`/`decode_stats` `dropped`/`drops` (or a `frame_drops` field) |
| decode timing | `diag`/`decode_stats` `p50_latency_ms`/`p95_latency_ms`/`fps`/`frames` |
| IPC | `diag`/`transport_stats` `p95_latency_ms`/`throughput_mbps` |
| A/V sync | peak absolute completion delta of concurrent `video_presented` / `audio_write_completed` events |
| audio scheduling skew (informational) | `media_timing.audio_scheduling_skew_ms` |
| audio latency | an event field `audio_latency_ms` |
| crashes | receiver exit status (non-zero, or death before the window ends) |

A metric with no source in the current build stays `null`. The wired receiver
emits a low-rate `media_timing` event carrying `audio_scheduling_skew_ms` (peak
`RouterStats::max_completion_skew`, informational only) and `audio_latency_ms`
(peak `RouterStats::max_path_latency`:
receiver input before decode to handoff-confirmed write completion at the
routing element). Both are the cumulative peak (worst observed) because the
phase-B gate is a run/soak budget. A final cumulative event is flushed before
router teardown, including orderly early exits. `av_sync_ms` is computed only
from the cross-stream consumer completion events: the video's retained common
monotonic timestamp must lie in the audio write's half-open scheduled interval.
The collector retains the maximum absolute paint-completion / stream-write-
completion delta over the window, regardless of log arrival order. No matched
pair means `null`, never a proxy or fabricated zero. These endpoints are API
completion events, not photon/speaker observations. Until an audio chunk is routed the fields
stay absent and the collector keeps them `null` -> the run is **BLOCKED** (fail
closed, never a fabricated `0`). Device-supplied text (thermal zone names) is
JSON-escaped as data and never injected into the document.

## Result document and provenance

The result embeds a `provenance` block with the source digest, build-artifact
digest, configuration digest, service definitions, dependency versions and the
target hardware state. `package_digest` is forced to `null` and
`equivalence_status` to `"provisional"`: post-package equivalence belongs to
todo 43, and the tool rejects a capture that claims otherwise. The report
validates its own output against `aa-projection-perf.schema.json` before
returning.

## Tests

```sh
ctest --preset host-dev -R wired_perf --output-on-failure
```

`wired_perf_schema`, `wired_perf_nominal`, `wired_perf_budget_fail`,
`wired_perf_duration`, `wired_perf_contract`, `wired_perf_collector`,
`wired_perf_media_timing` and `wired_perf_readiness` cover schema validation
(valid and malformed documents), the nominal PASS fixture, the failing-budget
fixture, strict duration parsing, the plan-mandated contracts, the collector
against a deterministic fake receiver, and the task-48 media-timing hook (the
collector picks up both fields, the value tracks the injected measurement, an
absent measurement stays BLOCKED). No live hardware is required.
