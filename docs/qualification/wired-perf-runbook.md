# Wired projection performance + soak runbook (task 32, phase B)

Phase-A acceptance for task 32 is the host-side tool, its schema, fixtures and
tests (`ctest -R wired_perf`), recorded in
`.omo/evidence/jetson-android-auto-receiver/task-32-wired-performance.json`.
This document is the **phase-B procedure**: the real 30-minute performance run
and the separate 2-hour soak on `jetson.local`.

> Phase B is owner/orchestrator-controlled and was NOT run during phase A (the
> Jetson display is DISCONNECTED: the HDMI/DP audio sink node 47 is absent).
> **No performance or soak number is claimed until the two live runs below
> produce result documents.** A green host test is not a live measurement.

## Tool

`tools/perf/aa-projection-perf.sh` (see `tools/perf/README.md`). It launches the
installed receiver (and the dummy display) over a bounded ssh, samples the
receiver process and thermal zones, parses the receiver's event stream, and
evaluates the contractual budgets. A budget miss exits 1 and is named in the
result JSON; an unavailable gated metric exits 3 (BLOCKED). The budgets are
never weakened.

## Prerequisites (once per target)

1. Task-29 udev policy installed:
   `sudo -n bash tools/usb-policy/install.sh` (matched ids on port `1-2.1`).
2. Approved phone connected on port `1-2.1` (phone mode `04e8:6860` before the
   AOA switch; `18d1:2d00`/`18d1:2d01` after) with Android Auto / USB debugging
   as needed, and the approved-phone trust store loaded (task 27).
3. Display connected (positive `connected` evidence from a DRM connector status
   **or** a connected output in `xrandr --query` on the session `DISPLAY=:1`)
   and a PipeWire output sink
    present (`pactl list short sinks` or `pw-cli`) — required for the audio-latency and
   A/V-sync budgets to be meaningful.
   nvidia-drm may leave sysfs at `disconnected` despite a working session output;
   xrandr's positive evidence is accepted. Absent/failed probes or all sources
   disconnected provide no positive evidence and BLOCK on `display_disconnected`.
4. An installed `aa-wired-receiver` (and `aa-dummy-display`) on the target. Until
   the aarch64 receiver/adapter packaging (task 41) lands, the target binary may
   be unavailable; the run is then BLOCKED on `receiver_binary_absent` (see
   `docs/qualification/wired-live-qa.md`, task-30 limitation).
5. The phone and display must stay connected and undisturbed for the whole run
    (30 minutes; then 2 hours for the soak). A disconnect invalidates the run.
   The collector probes DRM/session output connection at every sampling boundary,
   including the final boundary; loss of all positive evidence latches
   `display_connected: false` and a `display_disconnected` run violation even if
   the display process survives. Missing connection evidence blocks live qualification.

## Exact invocations

From the development checkout (bounded ssh, fail-closed):

```bash
# Bound, read-only prerequisite check (safe to run anytime).
tools/perf/aa-projection-perf.sh --readiness-only --out /tmp/task-32-readiness.json

# 30-minute performance run.
tools/perf/aa-projection-perf.sh --mode wired --duration 30m \
    --out /tmp/task-32-perf-30m.json --capture-out /tmp/task-32-capture-30m.json

# Separate 2-hour soak run (fresh invocation; do not reuse the 30m capture).
tools/perf/aa-projection-perf.sh --mode wired --duration 2h \
    --out /tmp/task-32-soak-2h.json --capture-out /tmp/task-32-capture-2h.json
```

Optional overrides: `--host` (default `jetson.local`), `--port` (default
`1-2.1`), `--receiver` (default `/opt/aa/aa-wired-receiver`), `--display`
(default `/opt/aa/aa-dummy-display`), `--store`, `--interval`.

If a prerequisite is absent the tool writes a `BLOCKED` result and exits 3, with
`blocked_on` naming the reason (`ssh_unreachable`, `receiver_binary_absent`,
`display_disconnected`, `no_audio_sink`, `collector_failed`,
`collector_produced_no_capture`, or `unavailable_metric:<name>`). The display or
phone being absent is therefore an explicit, non-zero BLOCKED outcome — never a
green smoke.

## What to record in the receipt

### Active window and continuity

Startup has a bounded 60-second grace (not counted toward qualification).
The requested 30-minute/2-hour window starts only after `active` and positive
receiver video/audio counters **and** video paint/audio write completions with
matched A/V pairs. A capture without that evidence is BLOCKED, not timed as a
successful projection. `startup_wait_seconds` and `active_projection_seconds`
are recorded separately from process lifetime (`duration_seconds`).

At least every five seconds the collector samples those cumulative counters.
Every stream, including the matched-pair counter, must advance within **10
seconds**. A longer freeze, counter reset, inactive projection, missing progress
after startup, or sampling gap over 10 seconds invalidates the run; later
recovery cannot heal it. The reporter independently replays these samples, so
a long-lived but stalled process cannot qualify. Connection loss also invalidates
the window. Use a continuously playing audio/video workload for qualification;
intentional media pauses do not count as verified active projection.

Both runs write a result document (validated against
`tools/perf/aa-projection-perf.schema.json`) whose gated metrics are compared to
the contractual budgets:

- receiver RSS peak <= 250 MB; receiver CPU sustained <= 50 % of one core;
- RSS growth < 10 %; A/V sync <= 50 ms; audio latency <= 200 ms; zero crashes;
- helper RSS recorded `null`;
- frame drops, decode timing, IPC timing and thermal state recorded
  informationally.

The 30-minute run establishes in-run budgets. The 2-hour soak establishes zero
crashes, RSS growth below 10 % and the per-metric ceilings. Copy the two result
documents (or their key verdicts) and the capture hashes into the phase-B
`live_qa` slots of the task-32 receipt.

## Budget caveats carried from task 9 (do NOT claim)

Task 9 is reduced-scope (owner-signed): the decode p95 latency budget
(<= 33 ms at 1280x720@30) and the software H.264 fallback qualification are
**withdrawn**. The tool records decode timing informationally and never asserts
that budget; the receipt must not claim it.

## Named limitations (completeness gaps, do NOT claim complete qualification)

### A/V measurement endpoints

`av_sync_ms` is the maximum absolute difference between **video paint completion**
(after `QPainter::end` in `ProjectionWidget`, once per consumed decoded frame)
and **audio stream-write completion** (the handoff-confirmed PipeWire routing
write used by task-25 `RenderBarrierStats`). Both use the common receiver-local
monotonic A/V clock. Video source/presentation timestamps are retained through
`VideoOutlet` -> video IPC -> decode correlation -> widget pixel queue. The
collector matches a video timestamp to the audio write's half-open scheduled
presentation interval, then compares the two observed completion timestamps.
Log arrival order does not define concurrency. No matched pair means BLOCKED.

These are **consumer-side completion events**, not photon/speaker observations:
Qt/compositor/display scanout and downstream PipeWire/device buffering are not
observed. This is the same stream-write endpoint vocabulary as `audio_latency_ms`.
The old audio-only completion-versus-schedule maximum is emitted honestly as
`audio_scheduling_skew_ms` (informational); it never satisfies the 50 ms A/V gate.

### Video measurements (task-32 additive emitters)

The receiver/display pair now emits the seven formerly missing video metrics.
This removes the **missing-emitter implementation gap**, not the need to run
the final candidate for 30 minutes and separately for two hours. No live
performance or software-decoder qualification is implied by host tests.

| Metric | Exact observed source and scope |
| --- | --- |
| `frame_drops` | Cumulative `VideoOutlet` AU decisions: no consumer, producer backpressure, or producer waiting for IDR. Not inferred from fragment gaps; not decoder/compositor/phone loss. `decode_stats.frame_drops` is absent until an AU is observed; a measured zero after successful input is valid. |
| `decode_p50_ms`, `decode_p95_ms` | Nearest-rank percentiles of matched `video_decode_completed` deltas: local `DecodeHelper::submit()` admission-entry `submitted_at` through mapped, dimension-checked, copied I420 output in `pump()`, before either completion or frame callback. Includes allocation, parser/decoder/conversion, pipeline and caller pump delay; NOT isolated hardware execution time. |
| `decode_frames` | Cumulative number of unique matched decoded output completion events across helper sessions. Unmatched/expired/reset correlations supply neither a frame measurement nor a duration. |
| `decode_fps` | Matched output count divided by measured elapsed time from earliest contributing submit entry to latest observed I420 completion. Not negotiated FPS and not a schedule-based estimate. |
| `ipc_throughput_mbps` | Unique successfully consumed/reassembled encoded payload bytes times 8, divided by the elapsed consumer window (earliest contributing connection attempt to latest receive completion), in decimal Mbit/s. Excludes header bytes, incomplete/rejected/suppressed AUs and decode time. Includes connection startup and inter-connection idle time. Does not require a matching producer log. |
| `ipc_p95_added_latency_ms` | Nearest-rank p95 of matched `video_ipc_send`/`video_ipc_receive`: producer timestamp immediately before the first fragment `send()` attempt through consumer full validated reassembly completion, before stream capture/decode. Includes remaining fragmentation/copies, socket queuing, receive polling and reassembly; excludes upstream AV dispatch and decode. |

All duration endpoints use same-host Linux `steady_clock` nanoseconds. The
existing wire `timestamp_ns` remains the unchanged common A/V timestamp and is
**only an opaque correlation key here**, never an IPC latency start or guessed
peer-clock conversion. The 56-byte v1 header, reserved words and limits exchange
are unchanged. Successful producer events are emitted after the last send using
the retained first-send timestamp; partial/error/backpressure/suppressed sends
have no successful endpoint. Consumer events occur only on complete AUs.

The collector joins `(producer_pid, consumer_pid, frame, first_fragment_sequence,
timestamp_ns)`, checks equal payload sizes, forward endpoint time and compatible
connection epochs, and binds each producer/consumer epoch one-to-one. Receiver
`VideoOutlet` frame IDs remain increasing across consumer reconnects; sequence
resets alone cannot match old frames. Consumer connect-attempt time must precede
the producer's post-handshake connection epoch and the send. A new epoch after an
old send therefore cannot borrow its endpoint. Logs may arrive in either order.

`aa_video_measurements.py` retains at most 4096 pending events per side, 4096
deduplication/binding entries per table, and 4096 deltas per metric. Pending IPC
events expire at ten seconds of event-time age (an operational retention bound,
not a performance budget); capacity eviction advances a conservative time floor
so a discarded endpoint cannot be revived by late replay. Quantiles describe
the last 4096 matched samples in processing order, not an exact whole-soak
distribution. Frame/byte counts and measured time bounds remain cumulative.
The capture's `video_telemetry` section records retained sample counts, unmatched
or expired IPC endpoints and final pending counts. Unpaired endpoints never
contribute zero latency. Invalid endpoint records latch the existing invalid
metric handling; missing data remains `null`, and all existing completeness and
budget gates stay unchanged.

Raw endpoint lines are flushed synchronously, so there is no periodic tail to
lose; orderly receiver close/destruction also flushes the final cumulative drop
counter. The collector's existing final log poll includes both streams after
termination. These additive diagnostics do add per-frame logging overhead,
which is part of the measured candidate. Decode telemetry remains informational:
the withdrawn 33 ms gate is **not** restored.

## Provenance and equivalence

Both result documents carry the provenance block with `package_digest: null` and
`equivalence_status: "provisional"`. Post-package equivalence is established in
todo 43, or the soak is rerun on the installed release. The tool rejects a
capture that claims otherwise.

**Live runs require populated provenance.** A live invocation (no `--capture`)
marks the document `measurement_origin: "live"` and will not report PASS unless
the deployment-bound `source_digest`, `build_artifact_digest` and
`configuration_digest` are SHA-256 values and `service_definitions` (name +
digest per service) and `dependency_versions` are non-empty. Pass them via
`--provenance <file>`; without them the verdict is BLOCKED and `provenance_gaps`
names each missing field. Offline `--capture` documents are labeled
`measurement_origin: "offline"` / `qualification_status: "OFFLINE_ONLY"` and can
never claim live qualification.

## Privacy

Device serials/MACs/IPs are never written to committed content or the receipt.
The collector records only target hardware state (machine, kernel, model, L4T
release, power mode, governor), not identifiers; raw captures stay under `/tmp`
on the host.
