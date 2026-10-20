# Wired projection performance + soak measurement (task 32)

`aa-projection-perf.sh` measures wired (USB/AOA) Android Auto projection
performance on the target Jetson and evaluates the **contractual** budgets.
It is the task-32 deliverable: phase A ships the tool, its JSON schema, its
fixtures/tests and the phase-B runbook; the actual 30-minute performance run and
2-hour soak are run later under orchestrator control (see
`docs/qualification/wired-perf-runbook.md`).

```
tools/perf/
  aa-projection-perf.sh            entry point (strict CLI, fail-closed)
  aa_projection_collect.py         on-target collector (launched over ssh)
  aa_video_measurements.py         bounded side-band video endpoint reducer
  aa_measurement_coverage.py       shared typed active-window evidence contract
  aa_perf_report.py                capture -> result document + budget verdict
  aa_perf_schema.py                stdlib JSON Schema validator (own output)
  aa-projection-perf-v2.schema.json current result-document contract
  aa-projection-perf.schema.json   unchanged historical v1 validation only
  README.md                        this file
```

## Contractual budgets

Fixed in `aa-projection-perf-v2.schema.json` and `aa_perf_report.py`. They are
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

## V2 active-window completeness

The reporter requires `aa-projection-perf-capture/2` and emits
`aa-projection-perf/2`; missing versions and historical v1 captures are not
promoted. Historical results can still be checked explicitly with
`aa_perf_schema.py validate tools/perf/aa-projection-perf.schema.json old.json`.
That is historical validation, not current qualification.

`measurement_coverage` is independently parseable after raw capture removal.
Its only window is the reporter's replayed active progress
`[started_ns,last_ns]`, in integer nanoseconds relative to the same-host
monotonic origin. The capture's active duration is cross-checked, never used as
an alternative window. Nominal sample spacing is <=5s; availability permits
gaps <=10s, including both edges. Missing starts have a null window, not
fabricated zero timestamps. Short or stalled active runs fail.

Every active RSS sample and entire first-active thermal inventory is required.
Thermal IDs combine sysfs directory and type; null, missing or changed inventory
blocks completeness. Negative finite temperatures are legal. RSS includes early
active breaches; growth needs actual first/last samples and a positive initial
value. CPU uses adjacent raw tick reads without bridging missing/decreasing
reads. Sustained CPU is duration-weighted and null unless the baseline and every
interval exist. Startup/shutdown resource readings never contribute.

Video evidence has one sealed summary per active sample interval for consumption,
IPC matches and decode. Edge-inclusive coverage retains whole-window counts,
bytes and gaps beyond 4096 observations. A ten-second event-time reorder horizon
seals whole sample intervals; old replay cannot repair sealed gaps. Capacity loss
latches `coverage_capacity`. Rings retain the last 4096 eligible processing-order
deltas with nearest-rank percentiles. Six decode/IPC event scalars are forbidden; they are
derived exclusively from evidence. Diagnostic `video_telemetry` never proves
completeness.

### Measurement cutoff versus terminal diagnostics

Immediately after `VideoMeasurements.finish(last_sample)`, before the receiver
grace wait, termination or final log drain, the collector snapshots all exported
event measurements: scalars, counters, startup timing and `helper_rss_mb: null`,
plus invalid-measurement markers and all `video_telemetry`. Samples, sealed video
evidence and progress retain their existing semantics. An event first consumed
after this cutoff cannot change measurements, even if its completion timestamp
is backdated into the prior window; sealed intervals cannot reopen.

Scalar peaks (including audio latency/skew and matched A/V), drops and counters
remain **run-cumulative observations admitted through the measurement cutoff**.
They are not newly startup-exclusive active-window metrics: startup maxima are
not subtracted and counter scope is not reset. RSS/CPU/thermal and video evidence
are reduced over the active window separately.

Terminal `err` records, crashes, actual exit status, early exit, unexpected
display death and unsuccessful bounded cleanup remain genuine failure information
through cleanup. Raw shutdown logs retain numbers for diagnostics only; a final
shutdown flush is not eligible measurement input. A pre-cutoff budget breach
still yields FAIL. Shutdown-only over-budget numbers change neither measurements
nor verdict, but a genuine failure can still make the verdict FAIL.

Missing RSS/CPU/thermal or any video stream yields BLOCKED/INCOMPLETE, without
adding throughput/FPS/thermal/decode budgets. Definite measured budget breaches
and failed runs retain FAIL precedence. Nominal/overload JSON fixtures are short,
synthetic **offline-only** v2 captures; separate tests synthesize 1800/7200-second
evidence. Neither proves a live run or hardware authenticity.

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
| frame drops | receiver `decode_stats.frame_drops`: run-cumulative observed outlet AU drops admitted through the measurement cutoff; shutdown-only final flush excluded |
| decode timing | matched `video_decode_completed` local submit-entry / I420-copy completion events |
| IPC | correlated `video_ipc_send` / `video_ipc_receive` endpoints; consumed payload bytes over measured consumer window |
| A/V sync | peak absolute completion delta of concurrent `video_presented` / `audio_write_completed` events |
| audio scheduling skew (informational) | `media_timing.audio_scheduling_skew_ms` |
| audio latency | an event field `audio_latency_ms` |
| crashes | receiver exit status (non-zero, or death before the window ends) |

A metric with no source in the current build stays `null`. The wired receiver
emits a low-rate `media_timing` event carrying `audio_scheduling_skew_ms` (peak
`RouterStats::max_completion_skew`, informational only) and `audio_latency_ms`
(peak `RouterStats::max_path_latency`:
receiver input before decode to handoff-confirmed write completion at the
routing element). Both are run-cumulative peaks (worst observed in events
admitted through the measurement cutoff), not startup-exclusive active-window
peaks. The receiver flushes a final cumulative event before router teardown,
including orderly early exits; if first consumed after the cutoff it is raw-log
diagnostic information only. `av_sync_ms` is computed only
from the cross-stream consumer completion events: the video's retained common
monotonic timestamp must lie in the audio write's half-open scheduled interval.
The collector retains the maximum absolute paint-completion / stream-write-
completion delta among pairs admitted through the cutoff, regardless of their
log arrival order before that cutoff. No matched pair means `null`, never a proxy
or fabricated zero. These endpoints are API completion events, not photon/speaker
observations; they do not qualify microphone input or physical output.
Until an audio chunk is routed the fields
stay absent and the collector keeps them `null` -> the run is **BLOCKED** (fail
closed, never a fabricated `0`). Device-supplied text (thermal zone names) is
JSON-escaped as data and never injected into the document.

Video endpoints preserve the v1 wire and A/V timestamp. The latter is an opaque
join key, never a send-start proxy. The reducer bounds pending/dedup tables and
rolling percentile deltas to 4096 entries, expires unmatched IPC at ten seconds,
and keeps cumulative eligible active-window frame/byte counts. Quantiles use
nearest rank over the last 4096 processed matched samples. See the runbook's **Video measurements**
section for exact endpoints, reconnect attribution, drop scope and exclusions.
The ssh stdin payload bundles the collector and reducer; no target-side module
installation is assumed. `video_telemetry` in the raw capture records retained,
unmatched and pending counts frozen at the measurement cutoff, not after cleanup.
Missing pairs are never zero-valued data.

Live qualification excludes a bounded 60-second startup grace: the requested
window begins only with active receiver video/audio progress, consumer paint/
write completions, and matched A/V pairs. Per-sample counters must keep advancing
within 10 seconds; freezes, counter resets, connection loss and sampling gaps
invalidate the run, even if both processes live. DRM connector status and the
session's `xrandr --query` are positive connection sources (either suffices;
neither means disconnected). The reporter independently verifies progress over
the entire window; startup-only evidence never qualifies. See the runbook for
endpoint limitations and the continuous-workload requirement.

Startup sequencing: `--video-socket`/`--input-socket` overrides are passed to
both the receiver and the display. With a managed display (`--display` and not
`--no-display`) the receiver is started with `--require-video-consumer` and the
display launches only once that receiver emits its `ready` event (listeners and
control infrastructure initialized, before version negotiation). The handshake
is bounded by the same absolute startup deadline measured from the monotonic
origin before launch - readiness never refreshes it. No `ready` event by that
deadline ends the run BLOCKED with the consumer unlaunched; receiver death
during the wait is an early exit. Readiness is the `ready` event alone: socket
existence, `ACTIVE` or a bare connection never substitute for it. Without a
managed display the receiver runs unchanged and no consumer gate is requested.

## Result document and provenance

The result embeds a `provenance` block with the source digest, build-artifact
digest, configuration digest, service definitions, dependency versions and the
target hardware state. `package_digest` is forced to `null` and
`equivalence_status` to `"provisional"`: post-package equivalence belongs to
todo 43, and the tool rejects a capture that claims otherwise. The report
validates its own output against `aa-projection-perf-v2.schema.json` before
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

`wired_perf_video_measurements` adds deterministic delta/byte/count/quantile,
reordering, duplicate, reconnect, stale and capacity cases, then drives the real
receiver outlet, AF_UNIX channel and software decode helper with the 90-frame
fixture and checks all seven fields through the actual collector parser. This
is host endpoint evidence only, not the 30-minute run or two-hour soak.

`wired_perf_v2_coverage` and `wired_perf_v2_capture` cover shared evidence,
adversarial summaries, missing resources/streams, sealing, edge gaps and synthetic
long windows. Run these independently of readiness or the full suite.
