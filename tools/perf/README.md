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
  aa_build_guard.py                refuses a stage from an unoptimized build or a
                                   binary that build did not produce
  aa_video_measurements.py         bounded side-band video endpoint reducer
  aa_measurement_coverage.py       shared typed active-window evidence contract
  aa_perf_report.py                capture -> result document + budget verdict
  aa_perf_schema.py                stdlib JSON Schema validator (own output)
  aa_av_contract.py                signed A/V contract (shared with the drive smoke)
  aa_moving_map.py                 moving-map workload (shared with the drive smoke)
  aa_soak_chain.py                 the 1800 s + 7200 s soak chain
  aa_remote_stop.sh                stops a collector run on the target and confirms
                                   its cleanup (phone restore fallback)
  aa-projection-perf-v3.schema.json current result-document contract (v3)
  aa-projection-perf-v2.schema.json historical v2 results (A/V max <= 50 ms)
  aa-projection-perf.schema.json   historical v1 validation only (build block optional)
  README.md                        this file
```

## Contractual budgets

Fixed in `aa-projection-perf-v3.schema.json` and `aa_perf_report.py` (`BUDGETS`;
v2 results keep `BUDGETS_V2` and the v2 schema). They are never weakened and a
diagnostics-suppressing flag does not exist:

| Metric | Budget | Gate |
| --- | --- | --- |
| receiver RSS | <= 250 MB (peak) | FAIL on miss |
| receiver CPU | <= 50 % of one core (sustained) | FAIL on miss |
| RSS growth over the run | < 10 % | FAIL on miss |
| A/V, every 5 s window mean | within ±40 ms (`av_window_mean_abs_ms`) | FAIL on miss |
| A/V, p99 \|skew\| | <= 50 ms (`av_p99_abs_ms`) | FAIL on miss |
| A/V excursions (\|skew\| > 90 ms) | at most floor(active s / 1800) pairs (`av_excursions`) | FAIL on miss |
| A/V hard cap | no pair over 150 ms (`av_hard_cap_pairs`) | FAIL on any |
| A/V, max \|skew\| | (`av_max_abs_ms`) | informational in v3 (owner decision 2026-11-17) |
| A/V audio coverage | >= 90 % of the active window (`av_coverage_pct`) | FAIL on miss, including no audio |
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

## Build guard

The run measures the deployed binaries, so the build type is part of the
measurement. Before the receiver or the display starts, `aa_build_guard.py`
reads `CMAKE_BUILD_TYPE` from the `CMakeCache.txt` of the build directory named
by `--build-dir` (or `AA_BUILD_DIR`) and refuses an unoptimized build with
`BLOCKED unoptimized_build` (exit 3). An unoptimized pixel conversion on the GUI
thread delays paint and the 5 ms display pump enough to fail the 50 ms A/V gate
on its own. `Release`, `RelWithDebInfo` and `MinSizeRel` pass
(case-insensitively); `Debug`, an unset value, an unreadable cache and a missing
build directory are refused. `--allow-unoptimized-build` is a debugging
override: it prints a warning and its runs are not qualification results. No
budget value changes.

Naming a build directory is not enough by itself: the guard also ties it to the
binaries this run launches. A binary inside the build directory (symlinks
resolved) is accepted; a copy deployed elsewhere is accepted only when it is
byte-identical to a binary that build produced; anything else is
`BLOCKED build_dir_mismatch` (exit 3). A cross build names the build tree on the
host that built it: `aa-projection-perf.sh` checks that local `CMakeCache.txt`
and derives each launched binary from the tree before anything is launched
remotely, and the capture records `build.verified_on: "host"` instead of a
target-side check it could not make.

Every run records the build in the capture and in the result JSON:
`build.build_type`, `build.build_dir`, `build.allow_unoptimized_build`,
`build.verified_on` and `build.non_qualifying`, plus the resolved binary paths
when they were verified. A run the override admitted is
`build.non_qualifying: "NOT_QUALIFYING: unoptimized_build"`, is reported
`BLOCKED` with that `blocked_on` instead of `PASS` (a measured budget miss still
`FAIL`s), and keeps its measured values visible. The collector writes those
lines to stderr and the launcher keeps them in the run output even on success, so
the build record is never silent.

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

The sole opt-in exception is `--thermal-policy orin-nano-cv-unavailable/1`,
bound before launch to the exact approved Orin Nano model. It retains all nine
raw zones and allows null only for CV0/1/2; continuous unique CPU/GPU/SoC0/1/2/TJ,
other zones, stable inventory and cadence remain required. Numeric CV readings
still contribute to peaks. Capture `thermal_policy` and exported
`measurement_coverage.thermal_scope` disclose the operational approval, model,
scope and numeric/unavailable counts. Nano results also retain a result-level
`thermal_policy` equal to the scoped policy, independently of `thermal_scope`;
export validation rejects either binding missing or disagreeing, even when
intermittent CV readings yield numeric peaks. Preserve both bindings after
removing the private capture pointer. The distinct scoped coverage policy is
not a generic full-all-zone claim. Omission/`strict-all-zones` retains the shipped
strict v2 layout. Offline `--thermal-policy` checks capture selection; it never
adds a waiver. See the runbook's **Explicit owner-approved Orin Nano thermal
availability policy** for the actual approval, precise scope and probe
limitations. No budget changes or causal
power-gating/sensor-absence claims are made.

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
tools/perf/aa-projection-perf.sh --mode wired --duration 30m --media-uri "$MEDIA" --out /tmp/perf-30m.json
tools/perf/aa-projection-perf.sh --mode wired --duration 2h  --media-uri "$MEDIA" --out /tmp/soak-2h.json
tools/perf/aa-projection-perf.sh --readiness-only --out /tmp/readiness.json
tools/perf/aa-projection-perf.sh --mode wired --duration 30m \
    --capture capture.json --out result.json     # offline: report a capture
```

Every live stage also needs `--drive-smoke RESULT` (or `AA_DRIVE_SMOKE`) and
`--phone-serial SERIAL` (or `AA_PHONE_SERIAL`): a passing
`tools/wired-qa/aa_drive_smoke.py` result from that same phone (attached on
`--port`), for the same receiver/display sha256, the default video offer and
configuration and the full 600 s, at most 24 h old (#137). Without one the
stage is `BLOCKED drive_smoke_*` (exit 3) before the collector starts. See the
runbook's **Drive smoke before any soak**. The soak chain is the 1800 s and
7200 s stages; the 60 s stage is retired.

`--duration` is strict: `<int>[smh]?` (no unit = seconds). Anything else is a
usage error (exit 2). `--mode` must be `wired`. `--host` (default
`jetson.local`) and `--port` (default `1-2.1`) select the target; `--receiver`
and `--display` name the installed binaries. Every ssh is wrapped in `timeout`.

The remote X session comes from `--display-env DISPLAY[,XAUTHORITY]`, then
`AA_DISPLAY`/`AA_XAUTHORITY`, then the caller's `DISPLAY`/`XAUTHORITY`; `:1` is
only the fallback (it is the display of a manually started second session, not
the owner's session). The same environment is used by `--readiness-only`, so the
`xrandr` evidence and the measured display are the same one.

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
| A/V sync | peak absolute schedule-to-completion latency difference of concurrent `video_presented` / `audio_write_completed` events; audio completes at `presented_ns` (#60, owner-approved 2026-11-08: when the chunk starts playing, max(handoff, its slot on the sink's clock from the element's position query)). Receivers on this contract log the key on every write, `null` when the sink gave no slot; a whole legacy log without the key keeps the `completed_ns` handoff. A `null`/malformed value, or a run mixing writes with and without the key, invalidates A/V rather than mixing endpoints |
| audio scheduling skew (informational) | `media_timing.audio_scheduling_skew_ms` |
| audio latency | an event field `audio_latency_ms` |
| audio stall dropouts (informational) | `audio_stall_bypass` `phase:"end"` events with outcome `write_returned` or `sink_reopened`: sink stalls survived by dropping audio (#60), and the `dropped` + `discarded` + `partial` chunks every ended stall cost (`partial`: some frames reached the sink before the rest was cancelled); in-flight chunks the blocked write fully delivered are reported as `rendered`, not lost |
| audio catch-up loss (informational, v3) | `audio_catch_up` events (#154): the receiver dropped a placement backlog in one run because the run would have played more than the 90 ms catch-up watermark behind its arrival (typically the phone's burst after a sink stall). Each record carries the render worker's running totals over the receiver's life, `events_lifetime` and `frames_lifetime` (output-rate frames removed beyond the ordinary bounded drift trim; that 5 ms-per-run allowance is not included), which survive a sink reopen (`frames_total` restarts there) and restore any earlier record the rate-limited media log dropped. `audio_catch_up_events` / `audio_catch_up_frames` are the newest record's totals and `audio_catch_up_ms` is those frames as duration at the logged `rate`. A `media_telemetry_dropped` notice after the newest catch-up record (a lost record no later one restores), a malformed record, or a catch-up record or notice cut off mid-line makes all three null: unknown, never undercounted. The totals freeze at the measurement cutoff, but the receiver reports a drop only with a later media record, possibly as it stops: a positive notice (or cut-off record) read only in the collector's final poll after the receiver exits also nulls the frozen totals. Only a drop the receiver never reports (killed before its next media record) cannot be seen. Never counted as an `audio_stall_*` dropout and never gated; a v3 capture recorded before the event existed reports null |
| unrecovered audio stalls (informational) | `audio_stall_bypass` ends with any other outcome (`reopen_failed`, `stopped`), plus a bypass still open when the capture ends |
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
diagnostic information only.

`media_timing` also carries the router's loss and silence accounting
(informational, never gated), summed over every sink route of the receiver's
life (`sink_reopens` counts the routes a reopen replaced): `frames_decoded`,
`frames_trimmed` (drift trims, catch-up included), `frames_caught_up` (the
catch-up part), `frames_silence` (synthesized gap frames), `frames_skipped`
(elapsed time advanced without a write: withheld credit or a playout
underrun's hole) and `frames_idle_skipped` (settled all-role idle), all in
48 kHz output frames. When the playout clock is enabled (default on both
wireless, `120` ms, owner decision 2026-11-20, and wired, `60` ms, owner
decision 2026-11-23 / #228), `media_timing` adds the playout buffer and
credit window fields: `playout_target_ms` (the configured depth),
`playout_depth_ms` (the latest chunk's slot minus its arrival),
`playout_depth_min_ms` / `playout_depth_max_ms` (that depth's range, anchors
excluded; `null` before the first steady chunk), `playout_offset_ms` (the
depth of the least-delayed recent arrivals: how long after on-time content
audio plays, the target of `audio_write_completed`'s `playout_offset_ns` and
what the wireless video hold follows; above the depth by the phone's group
span), `playout_underruns` (a chunk up to 1 s
after its slot), `playout_anchors` (stream starts and resumes) and
`playout_resumes` (anchors more than 1 s after the slot),
`playout_reported_offset_ms` (the slewed offset the latest write reported:
the value the wireless video hold follows; it moves toward
`playout_offset_ms` by at most half the output time that passes),
`frames_padded` (drift pads, a subset of `frames_silence`), `frames_shed`
(a backlog whose offset is above min(depth + 230 ms, 350 ms) shed at 30% of
each chunk, a subset of
`frames_trimmed`), `playout_shed_residual_chunks` (chunks whose committed
offset still exceeded the shed target, including when the buffer minimum
prevented further shedding), `playout_shed_residual_peak_ms` (the largest such
remaining excess), `credit_window` (the advertised
`max_unacked`), `max_unacked_held` (the most chunks one audio channel had
accepted and not yet released for ACK: our side, normally 1 because acks go
out on receipt) and `max_arrival_burst` (the most chunks of one channel that
arrived within 10 ms of each other: the phone's side, above 1 when it uses
more than one credit at once). Each underrun also logs
`audio_playout_underrun` with `underruns_lifetime`, `late_ms` (how long after
its slot the chunk arrived) and `depth_ms` (the depth it re-anchored with),
and each anchor logs `audio_playout_anchor` with `reason` (`start` or
`resume`), `skipped_ms` (elapsed time jumped over without a write),
`depth_ms` and `anchors_lifetime`. The wireless VideoOutlet logs
`video_hold_capped` (`requested_ms`, `cap_ms` 400, `capped_frames`, counted
when a capped frame is released) when the offset it follows exceeds its hold
cap, and `video_release_late` (`late_ms`, `late_releases`, `by`: the path that
released it, `timer`, `checkpoint`, `arrival` or `service`, and
`checkpoint_gap_ms`: how long the owner thread's checkpoint had not run) when
a held frame leaves more than 20 ms after its due. Held frames are released
by the outlet's own timer thread at their due, and also at checkpoints; a
delivery error on the timer is returned by the session's next outlet call, and
`video_release_timer_error` (`reason`: `exception` or `start_failed`) records
a timer that threw or could not start. `playout_reported_offset_ms` keeps its
last value when the stream stops; only the history the video lookup uses is
cleared.

On the playout clock (default on both wired and wireless) `audio_write_completed.scheduled_ns` and
`scheduled_end_ns` are the content's arrival-clock times and the additive
`playout_offset_ns` is how much later the piece was scheduled (the content
interval never starts before the previous one ended, so after an offset
increase the end offset briefly exceeds it while content advances at half
rate), so
`presented_ns - scheduled_ns` includes the playout buffer and the A/V pairing
matches a video frame with the audio that arrived with it. Both receivers
hold video for the reported offset (`--video-delay-ms match`), so the
signed A/V contract is unchanged (`aa-wired-receiver` logs `wired_media_policy`
and `aa-wireless-receiver` logs `wireless_media_policy` at startup). Records on
the legacy arrival model (`--audio-playout-ms 0`) never carry the field. `av_sync_ms` is computed only
from the cross-stream consumer completion events: the video's retained common
monotonic timestamp must lie in the audio write's half-open scheduled interval.
For each pair, skew is `(video completed - video scheduled) - (audio write
completed - audio chunk scheduled start)`: the difference between the two
streams' schedule-to-completion latencies. The frame's position inside the audio
chunk is therefore not counted as skew (owner-approved 2026-10-20; previously
the raw completion delta, which added up to one chunk duration). The collector
retains the maximum absolute skew among pairs admitted through the cutoff,
regardless of their log arrival order before that cutoff. No matched pair means `null`, never a proxy
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

The result embeds a `build` block with the verified `build_type`, the
`build_dir` it was read from, `allow_unoptimized_build`, `verified_on`
(`target`/`host`/`none`) and `non_qualifying` (the `NOT_QUALIFYING:` marker on a
run the debugging override admitted; such a run is never `PASS`).

It also embeds a `provenance` block with the source digest, build-artifact
digest, configuration digest, service definitions, dependency versions and the
target hardware state. `package_digest` is forced to `null` and
`equivalence_status` to `"provisional"`: post-package equivalence belongs to
todo 43, and the tool rejects a capture that claims otherwise. The report
validates its own output against its version's schema (v3, or v2 for a v2
capture) before returning.

Versions (#137): a v2 capture replays to a v2 result under the v2 contract
(A/V max <= 50 ms), never reinterpreted. Live stages write capture `/3`, whose
`av_series` (signed pairs and audio coverage) the reporter judges with the
signed A/V contract, and whose `workload` must prove the moving-map workload
ran: it started, no phone command failed, the drag rate met the floor, and no
gap was over 10 s. `--media-uri` is required for every live stage. A build seen
on the target during a stage (`concurrent_builds`) BLOCKS it. See the runbook's
**Soak stages: moving-map workload and signed A/V (v3)**.

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

`wired_perf_v2_thermal_policy` exercises strict-vs-Nano replay, all required
families, inventory changes, real CV peaks, nonfinite rejection, wrong models,
budget/terminal precedence, forged exports and both real offline CLI paths.

`wired_perf_build_guard` covers the qualification build guard: Debug/unset/
unreadable builds refused before anything launches, Release/RelWithDebInfo/
MinSizeRel accepted, the debug override admitted and recorded, launched binaries
tied to the build directory (in-tree, identical deployed copy, symlink escape
and foreign build refused, cross-build derivation), an override run reported
`NOT_QUALIFYING` instead of `PASS`, and the launcher's display-environment
resolution — all against real `CMakeCache.txt` fixtures and a PATH-local ssh
stand-in; no target is touched.

### Audio playback-slot diagnostics

Current receiver `audio_write_completed` events follow
[`audio-write-completed.schema.json`](audio-write-completed.schema.json).
`presented_ns` remains the real playback slot clamped to handoff completion;
null still invalidates A/V. The additive `playback_slot_absence` field is null
when a slot exists, otherwise one of:

| Reason | Meaning |
| --- | --- |
| `worker_busy` | Earlier position query is still in flight; no new query queued |
| `worker_stopped` | Query worker stopped or exited |
| `deadline_missed` | No matching answer within min(original write deadline, now + 20 ms), or budget already spent |
| `query_failed` | Sink missing or position query failed |
| `negative_position` | Position query returned a negative position |
| `invalid_pts` | PTS absent or outside the signed position domain |
| `unavailable` | Sink implementation supplies no playback endpoint |

The collector retains diagnostic counts as `playback_slot_absences` in the
capture, including unknown future reason strings. Historical events without
this field remain readable. Reasons never substitute a handoff endpoint or
change interval matching, null invalidation, or the 50 ms maximum A/V gate.

The audio route uses initial preroll at PTS zero (readiness retries use running
time once the clock settles), then requires settled PLAYING
and an identity TIME segment before admitting media. Its media epoch is
`base_steady = steady_now - (gst_clock_now - pipeline_base_time)`, with the
steady reading bracketing the clock sample; `PTS = scheduled_steady - base_steady`.
The mapping is checked before every media buffer and sampled again when the
pipeline base time or clock changes. Rebuilt routes establish a fresh mapping;
unsupported segments/states fail the route instead of guessing an epoch.
See [GStreamer's clock and segment model](https://gstreamer.freedesktop.org/documentation/application-development/advanced/clocks.html).
