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

## Build

Qualification and soak runs measure the deployed binaries themselves, so the
build type is part of the measurement: **both runs require an optimized build.**

```bash
# On the Jetson (native aarch64):
cmake --preset host-release && cmake --build --preset host-release
# Cross build: cmake --preset jetson-aarch64
```

Never the Debug `host-dev` or `host-asan` preset. `RelWithDebInfo` is acceptable
when a run needs symbols: it keeps optimization and only adds debug info.
Why: on one known-good commit, Debug runs measured 173.3 ms and 142.9 ms A/V
sync where the same commit measured 16.2 ms, 30.1 ms and 21.5 ms optimized, and
building only the projection widget's pixel conversion unoptimized reproduced a
106.8 ms failure against the 50 ms gate — that conversion runs on the GUI thread
and delays paint and the 5 ms display pump.

The collector enforces this: before it launches the receiver or the display it
reads `CMAKE_BUILD_TYPE` from the `CMakeCache.txt` of the build directory named
by `--build-dir` (or `AA_BUILD_DIR`) and refuses with `BLOCKED
unoptimized_build` when that build is not optimized, when no build directory is
given, or when the cache is unreadable. `Release`, `RelWithDebInfo` and
`MinSizeRel` are the optimized types (case-insensitive). `--allow-unoptimized-build`
exists only to debug the tooling: it prints a warning, its run is reported
`NOT_QUALIFYING: unoptimized_build` (and never `PASS`), and it is not a
qualification result. Record the `CMAKE_BUILD_TYPE` and the effective C++
compiler flags (`CMAKE_CXX_FLAGS*` from the same cache) in the run report.

The build directory must also be the one that produced the binaries the run
launches. Each of `--receiver`/`--display` is accepted when it lies inside
`--build-dir` (symlinks resolved) or is byte-identical to a binary that build
produced; anything else is `BLOCKED build_dir_mismatch`. So either launch the
receivers out of the build tree, or install an unchanged copy of exactly that
build. A cross build (`jetson-aarch64`) may name the build tree on the host that
built it: the launcher verifies that local `CMakeCache.txt`, derives each
launched binary from the tree, and the capture records
`build.verified_on: "host"`. Every capture and result records
`build.build_type`, `build.build_dir`, `build.allow_unoptimized_build` and
`build.verified_on`; keep them when copying a result into the receipt, and check
`build.non_qualifying` is `null` before claiming a qualification run.

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
   **or** a connected output in `xrandr --query` on the session display named by
   `--display-env` — see the display-session note below) and a PipeWire output
   sink present (`pactl list short sinks` or `pw-cli`) — required for the
   audio-latency and A/V-sync budgets to be meaningful.
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

Display session: with GDM automatic login the owner's session is on display
`:0`, with its X authority cookie at `/run/user/<uid>/gdm/Xauthority`. The
launcher resolves the remote X session in this order: `--display-env
DISPLAY[,XAUTHORITY]`, then `AA_DISPLAY`/`AA_XAUTHORITY`, then the caller's
exported `DISPLAY`/`XAUTHORITY`; `:1` is only the fallback, and it is the display
of a manually started second X session, not the owner's session. Qualification
needs the display the owner's screen session is actually on, so name it
explicitly:

```bash
tools/perf/aa-projection-perf.sh --mode wired --duration 30m \
    --display-env ":0,/run/user/1000/gdm/Xauthority" \
    --build-dir "$AA_BUILD_DIR" --out /tmp/task-32-perf-30m.json
```

and confirm the positive `xrandr --query` evidence comes from that session
before starting (`--readiness-only` uses the same environment). A
`DISPLAY`/`XAUTHORITY` value that is not a plain session path is a usage error;
the values are embedded in the remote command, so they are never passed through
a shell unquoted.

## Drive smoke before any soak (#137)

The soak workload is a near-static screen with a tone on one phone. The S24
Ultra video freeze (#134) showed up only with a moving map, so no soak stage
could catch it. **No soak stage (60, 1800 or 7200 s) starts without a passing
ten-minute moving-map drive smoke on the same build.**

`tools/wired-qa/aa_drive_smoke.py run` runs on the Jetson the phone is plugged
into. It refuses to start (`BLOCKED`, exit 3) on an unoptimized build, on
binaries that the named build did not produce, while a compiler or another
receiver or display is running, or when the phone is not on `--port`. It
then opens Google Maps on the phone with a `geo:` intent and launches
`aa-wired-receiver` and `aa-dummy-display` (on the owner's session display).
While the window runs it drags the map once a second over the display's test
control channel. Media is optional: pass it so A/V gets checked.

```bash
HU_KEY_PATH=<credential path> tools/wired-qa/aa_drive_smoke.py run \
    --serial <adb serial> --port 1-2.1 --build-dir "$AA_BUILD_DIR" \
    --display-env ":0,/run/user/$(id -u)/gdm/Xauthority" \
    --media-uri file:///sdcard/Music/<tone>.opus --media-type audio/ogg \
    --media-component org.videolan.vlc/org.videolan.vlc.gui.video.VideoPlayerActivity \
    --out "$XDG_RUNTIME_DIR/drive-smoke-$(date +%Y%m%dT%H%M%S)"
```

The drive is 600 s by default (`--duration`), measured from the first painted
frame. There is no first-paint deadline: the latency from `active` to the
first paint is reported as `measured.startup_to_first_paint_s`. The run writes
`receiver.log`, `display.log`, `idr-events.jsonl` (`idr_*` lines stamped on
arrival, because they carry no time), `run.json` and `result.json`.
`aa_drive_smoke.py analyze DIR` re-evaluates a directory offline. SIGTERM or
SIGINT only sets a stop flag, which the runner checks at every wait point. It
then runs the same single cleanup as a normal finish: it stops each registered
child's whole process group (TERM, then KILL), restores the phone's USB mode
and records the run as `BLOCKED interrupted` (exit 128+the first signal). A
second signal cannot interrupt that cleanup. Touch control goes to the
display through a non-blocking pipe, so a display whose GUI thread stops
draining it cannot block the runner: if no control input is accepted for
`CONTROL_WRITE_TIMEOUT_S` (2 s), the run ends as FAIL with
`failure: display control input stalled`, and any runner-detected failure
turns a PASS from the analyzer into FAIL. The pass criteria are named
constants in the tool:

| Constant | Criterion |
| --- | --- |
| `PAINTED_FPS_MIN_RATIO` = 0.90, `WINDOW_S` = 10, `FPS_STARTUP_GRACE_S` = 10 | every full 10 s window from first paint + 10 s to the end of the drive paints >= 90 % of the **negotiated** fps (59 windows in 600 s) |
| `WAITING_IDR_MAX_S` = 1.0 | no `waiting_idr` episode (producer or consumer) lasts > 1 s inside the drive. An episode starts at the earlier of its first stamped `idr_requested` and (`idr_recovered` − `latency_ms`). A recovery with no logged request still counts: the display emits one even when every request failed. When `latency_ms` is absent, negative, nonfinite or not a number, the episode is taken as open since `active`, even with a logged request, because the wait can predate its first successful request. An unrecovered episode runs from its first request to the window end. Every episode is intersected with [`active`, window end] |
| — | no session drop: both processes alive at window end, the full window ran, no `disconnected` session state and no `terminal_error` inside it |
| `AV_SYNC_MAX_MS` = the existing 50 ms budget | max A/V paint-vs-audio-write difference <= 50 ms, **where valid**: only when matched pairs exist (media playing); otherwise reported `n/a`. An audio slot whose `presented_ns` is null is skipped and counted in `invalid_audio_slots`. This differs from the perf collector, which invalidates the whole A/V metric for such a slot; the soak stages keep that stricter rule |

**Startup grace (fps only).** The first 10 s after the first paint are
excluded from the fps check, and only from it. The waiting_idr, session-drop
and A/V checks still cover the whole run from `active`. Why: in the 2026-11-16
A26 drive smoke the phone's encoder paused for 853 ms 0.1 s after the first
frame. It sent 267 frames in the first 10 s, and all 267 were received, decoded
and painted (26.7 fps against the 27 fps floor). Windows 1–59 painted
29.8–30.1 fps. A research run the same day painted 28.4 fps in that first
window, so without the grace the phone's start-up ramp decides the verdict by
chance. **This deviates from the #137 issue text ("per 10 s window"); the owner
may veto it.**

**The gate qualifies the receiver's default video offer**, whose primary
profile is `1280x720@30:h264` and which the soak stages also negotiate. Healthy
A26 runs on 2026-11-16 painted 29.8–30.1 fps per window at 720p30 after the
grace. At 1080p60
and 1440p60 the Orin Nano paints only about 50 and 36 fps even when healthy
(#79, #85), so the unchanged 90 % rule fails those profiles. A run with
`--video-profiles` is analyzed and reported as usual, but it can never satisfy
the soak gate.

The soak launcher enforces the gate. Every live `aa-projection-perf.sh` stage
needs `--drive-smoke <result.json>` (or `AA_DRIVE_SMOKE`). Before the collector
starts, the launcher hashes the receiver and display it is about to launch, on
the target, and requires:

- the record's verdict to be `PASS`;
- the same receiver and display sha256;
- the default video offer and the full 600 s duration;
- `finished_utc` within `RECORD_MAX_AGE_S` = 24 h **by the real clock**. The
  launcher's `--now` only dates the result document and is never used for
  this check; `aa_drive_smoke.py check --now` exists for tests and offline
  review only. The 60+1800+7200 s chain takes about 2.6 h, so this is a
  same-day rule that an overnight phone or Maps update cannot slip past.

The record is strict JSON (`NaN` and `Infinity` are refused), and every field
the gate reads must be present with the right type (`video_profiles` must be
present and `null`). Both binaries must hash on the target
to a 64-hex-digit sha256. A display that cannot be hashed, or a stage with no
display, is blocked.

Otherwise the stage is `BLOCKED` with `drive_smoke_missing`,
`drive_smoke_malformed`, `drive_smoke_hash_failed`, `drive_smoke_not_passed`,
`drive_smoke_build_mismatch`, `drive_smoke_profile`, `drive_smoke_short` or
`drive_smoke_stale`. Offline `--capture` replay and
`--readiness-only` do not need a record.

Gate budget (one gate out for each gate in): this gate proposes **retiring the
60 s soak stage**. It is a one-minute shakedown of the same static workload
that the 1800 s stage then repeats. The drive smoke now runs first, on the same
build, for ten minutes, with a moving workload. The 60 s stage stays in place
until the owner confirms the retirement.

## Exact invocations

From the development checkout (bounded ssh, fail-closed):

```bash
# Bound, read-only prerequisite check (safe to run anytime).
tools/perf/aa-projection-perf.sh --readiness-only --out /tmp/task-32-readiness.json

# 30-minute performance run (optimized build, see Build).
tools/perf/aa-projection-perf.sh --mode wired --duration 30m \
    --build-dir "$AA_BUILD_DIR" --drive-smoke "$DRIVE_SMOKE_RESULT" \
    --out /tmp/task-32-perf-30m.json --capture-out /tmp/task-32-capture-30m.json

# Separate 2-hour soak run (fresh invocation; do not reuse the 30m capture).
tools/perf/aa-projection-perf.sh --mode wired --duration 2h \
    --build-dir "$AA_BUILD_DIR" --drive-smoke "$DRIVE_SMOKE_RESULT" \
    --out /tmp/task-32-soak-2h.json --capture-out /tmp/task-32-capture-2h.json
```

Optional overrides: `--host` (default `jetson.local`), `--port` (default
`1-2.1`), `--receiver` (default `/opt/aa/aa-wired-receiver`), `--display`
(default `/opt/aa/aa-dummy-display`), `--store`, `--interval`, `--build-dir`
(default `AA_BUILD_DIR`), `--display-env DISPLAY[,XAUTHORITY]` (default: the
caller's `DISPLAY`/`XAUTHORITY`, else `:1`). `--build-dir` names the build
directory that produced the launched receiver/display — the run is BLOCKED
without it, and BLOCKED with `build_dir_mismatch` when those binaries are not
that build's (see Build).

### Explicit owner-approved Orin Nano thermal availability policy

Default `--thermal-policy strict-all-zones` (also the default when omitted) is
unchanged: any unavailable first-active thermal-zone reading blocks coverage.
Model detection alone never enables an exception.

The owner was asked: "May I revise qualification policy to retain those three
readings explicitly as unavailable, while requiring continuous CPU/GPU/SoC/TJ
and keeping every performance budget unchanged?" The actual reply was:
"Approve Nano-specific policy (but I highly doubt there is no tegrastat command
or other way...)". The approval is recorded as
`owner-approved-nano-cv-availability-20261018`; it authorizes only this explicit
availability exception, not fake temperatures or a universal Nano-model waiver.

For the approved operation only, add this flag to **each** separately authorized
live invocation above:

```text
--thermal-policy orin-nano-cv-unavailable/1
```

The collector checks the actual hardware model before launching anything. Only
the exact model `NVIDIA Jetson Orin Nano Engineering Reference Developer Kit Super`
is admitted; absent, unknown, other Jetson and approximate model names fail
closed. The observed probe platform was L4T R39.2.1, kernel
6.8.12-1021-tegra. This approval does not certify another deployment/release.

All nine CPU/GPU/CV0/CV1/CV2/SoC0/SoC1/SoC2/TJ zone identities remain in every raw
sample. Every required CPU/GPU/SoC0/SoC1/SoC2/TJ reading must be finite at every
active sample; each family must occur exactly once. Other discovered zones are
still required. Only the three canonical CV types may be null/unavailable;
every real numeric CV observation still contributes to its zone peak and the
overall thermal peak. No value is carried forward or mapped from another zone.
The first-active inventory remains stable, including directory/type identity;
disappearing CV directories latch `thermal_inventory_changed` even though their
raw entries are retained as null. Added/renamed identities still block. Cadence,
edge coverage <=10s, budgets, A/V, freezes, terminal failures and provenance are
unchanged.

The capture contains `thermal_policy` with explicit profile/model/approval/scope.
The result carries distinct coverage policy
`active-window-coverage/orin-nano-cv-unavailable/1` plus
`measurement_coverage.thermal_scope`: the same policy and per-zone counts of
numeric and unavailable active observations. Keep that entire block, the nine
zone peaks/nulls and the scope note when removing the private `capture` pointer
for export. Both qualification runs must have the same policy and match the
manifest target model. This is **not full-all-zone thermal qualification**.
The marker records this operational approval; it is not cryptographic approval
authentication or evidence that a live run occurred.

Replay reads selection from the capture, never from the current machine. An
optional `--thermal-policy` on `--capture` requires a matching existing marker;
it cannot opt an old strict capture into Nano policy. Historical shipped v2
strict captures/results retain their original contract and layout.

Read-only probe receipts are privately retained in
`.omo/evidence/jetson-android-auto-receiver/live-unblock-20261018/thermal-probe/`.
Sysfs CV reads returned ENODATA; four foreground tegrastats samples contained the
six required families but no CV temperature fields, and hwmon exposed only NVMe
temperature. No alternative CV measurement was demonstrated. Neither physical
sensor absence nor a power-gating cause was proven; the approval is not such a
finding. Future real CV measurements must be retained, not overridden by null.

If a prerequisite is absent the tool writes a `BLOCKED` result and exits 3, with
`blocked_on` naming the reason (`ssh_unreachable`, `receiver_binary_absent`,
`display_disconnected`, `no_audio_sink`, `unoptimized_build`,
`build_dir_mismatch`, `drive_smoke_*` (see Drive smoke), `collector_failed`, `collector_produced_no_capture`,
`unavailable_metric:<name>`, or `NOT_QUALIFYING: unoptimized_build` for a run
the qualification override admitted). The display or
phone being absent is therefore an explicit, non-zero BLOCKED outcome — never a
green smoke.

## What to record in the receipt

### Active window and continuity

Startup has a bounded 60-second grace (not counted toward qualification).
The requested 30-minute/2-hour window starts only after `active` and positive
receiver video/audio counters **and** video paint/audio write completions with
matched A/V pairs. A capture without that evidence is BLOCKED, not timed as a
successful projection. `startup_wait_seconds` and `active_projection_seconds`
are recorded separately from process lifetime. V2 result `duration_seconds` is
exactly the replayed active window; capture `observed_duration_seconds` is only
a process-lifetime diagnostic.

At least every five seconds the collector samples those cumulative counters.
Every stream, including the matched-pair counter, must advance within **10
seconds**. A longer freeze, counter reset, inactive projection, missing progress
after startup, or sampling gap over 10 seconds invalidates the run; later
recovery cannot heal it. The reporter independently replays these samples, so
a long-lived but stalled process cannot qualify. Connection loss also invalidates
the window. Use a continuously playing audio/video workload for qualification;
intentional media pauses do not count as verified active projection.

A collector sampling gap is host interference, not a media stall (#90). Every
gap over 7 seconds is recorded as `interference` in the capture and in the
result's `run_status`. After a gap over 10 seconds the collector re-polls the
receiver log for up to 5 seconds, then judges from the receiver's own logged
event times: every stream needs events through the gap with no silence over 10
seconds, otherwise it is reported as `media_stalled`. Either way a gap over 10
seconds still fails `coverage:sampling`, so an interrupted run is never a PASS;
the interference entry explains why. `video_units` and `audio_chunks` log no
timestamp, so the video and audio completion events stand in for their timing.
For every sample whose stall decision depended on receiver-logged evidence
(both >10 s gaps and post-gap event-anchored samples), the collector persists
that exact evidence keyed by `t_ns` in `progress_evidence` so offline replay
matches the live collector; older captures without `progress_evidence` remain
loadable (falling back to `interference[*].logged_progress` when present).

### Measurement cutoff and cleanup diagnostics

Immediately after `VideoMeasurements.finish(last_sample)`, before receiver grace
wait, termination and the final log drain, all exported event measurements are
snapshotted: scalar metrics, counters, startup timing and `helper_rss_mb: null`,
invalid-measurement markers and all `video_telemetry`. Existing samples, sealed
video evidence and progress are unchanged. An event first consumed after that
cutoff cannot update measurements even if its completion timestamp is backdated
into the prior window. No sealed interval reopens or gains late coverage.

Scalar peaks (audio latency/skew and matched A/V included), drops and counters
remain **run-cumulative observations admitted through the measurement cutoff**,
not newly startup-exclusive active-window metrics. Do not subtract startup maxima
or reset counter scope. RSS/CPU/thermal and video evidence retain their separate
active-window reduction and completeness rules.

The final drain still preserves terminal `err` records and lifecycle diagnostics.
Crashes, actual exit status, early exit, unexpected display death and unsuccessful
bounded cleanup remain genuine failure information through cleanup. Raw logs may
contain shutdown numbers, including final cumulative media-timing/drop flushes;
these are diagnostic only when first consumed after the cutoff, not eligible
measurements. A pre-cutoff over-budget observation still yields FAIL. A
shutdown-only over-budget value changes neither metrics nor verdict, whereas a
genuine failure during cleanup can still produce FAIL.

Both runs write a result document (validated against
`tools/perf/aa-projection-perf-v2.schema.json`) whose gated metrics are compared to
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
Log arrival order does not define concurrency, but both events must be admitted
through the measurement cutoff; late first consumption cannot add a pair even
with backdated completion timestamps. The maximum remains run-cumulative through
that cutoff, not startup-exclusive. No matched pair means BLOCKED.

These are **consumer-side completion events**, not photon/speaker observations:
Qt/compositor/display scanout and downstream PipeWire/device buffering are not
observed. This is the same stream-write endpoint vocabulary as `audio_latency_ms`.
These observations do not qualify microphone input or physical output.
The old audio-only completion-versus-schedule maximum is emitted honestly as
`audio_scheduling_skew_ms` (informational); it never satisfies the 50 ms A/V gate.

### Video measurements (task-32 additive emitters)

The receiver/display pair now emits the seven formerly missing video metrics.
This removes the **missing-emitter implementation gap**, not the need to run
the final candidate for 30 minutes and separately for two hours. No live
performance or software-decoder qualification is implied by host tests.

| Metric | Exact observed source and scope |
| --- | --- |
| `frame_drops` | Run-cumulative `VideoOutlet` AU decisions admitted through the measurement cutoff: no consumer, producer backpressure, or producer waiting for IDR. Not inferred from fragment gaps; not decoder/compositor/phone loss. `decode_stats.frame_drops` is absent until an AU is observed; a measured zero after successful input is valid. Shutdown-only final flushes are excluded. |
| `decode_p50_ms`, `decode_p95_ms` | Nearest-rank percentiles of matched `video_decode_completed` deltas: local `DecodeHelper::submit()` admission-entry `submitted_at` through mapped, dimension-checked, copied I420 output in `pump()`, before either completion or frame callback. Includes allocation, parser/decoder/conversion, pipeline and caller pump delay; NOT isolated hardware execution time. |
| `decode_frames` | Cumulative number of eligible active-window unique matched decoded output completion events across helper sessions, frozen at the measurement cutoff. Unmatched/expired/reset correlations supply neither a frame measurement nor a duration. |
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
distribution. Eligible active-window frame/byte counts and measured time bounds
remain cumulative within that window and freeze at the measurement cutoff.
The capture's `video_telemetry` section records retained sample counts, unmatched
or expired IPC endpoints and pending counts at the cutoff, not after cleanup.
Unpaired endpoints never contribute zero latency. Invalid endpoint records latch the existing invalid
metric handling; missing data remains `null`, and all existing completeness and
budget gates stay unchanged.

Raw endpoint lines are flushed synchronously, so there is no periodic tail to
lose; orderly receiver close/destruction also flushes the final cumulative drop
counter, which is diagnostic only if first consumed after the measurement cutoff.
The final log poll retains terminal/lifecycle diagnostics after termination,
but all measurements are already frozen: shutdown endpoints cannot extend counts,
bytes, rings, scalar peaks or telemetry, change invalid-measurement markers, or
repair coverage. These diagnostics add per-frame logging overhead,
which is part of the measured candidate. Decode telemetry remains informational:
the withdrawn 33 ms gate is **not** restored.

## Provenance and equivalence

Both runs require capture `aa-projection-perf-capture/2` and result
`aa-projection-perf/2`, with `measurement_coverage` policy
`active-window-coverage/1`. V1 results remain explicitly validatable against
their unchanged schema, not automatically qualified as v2. Independently parse
coverage after export removes the capture: labels/hashes alone prove neither
completeness nor hardware authenticity.

Require sampling/RSS/thermal at both active edges and maximum gaps <=10s;
every CPU interval with a valid initial baseline and measured duration E-S;
and nonempty consumption/match/decode streams with edge-inclusive gaps <=10s.
Thermal needs the stable first-active inventory at every sample; empty inventory
is not zero temperature. Rates require positive denominators. Rings must contain
`min(4096, stream count)` deltas and use nearest rank in eligible processing order;
sealed interval summaries retain whole-window counts/gaps beyond that ring.
Missing required evidence yields BLOCKED/INCOMPLETE, but measured breaches,
crashes and short/frozen progress retain FAIL precedence. These availability
rules add no decode-33ms, throughput or FPS budget.

No actual 30-minute or two-hour run is started by these changes or synthetic
tests. Both live acceptance runs remain outstanding.

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
