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
could catch it. **No soak stage (1800 or 7200 s) starts without a passing
ten-minute moving-map drive smoke on the same build.**

`tools/wired-qa/aa_drive_smoke.py run` runs on the Jetson the phone is plugged
into. It refuses to start (`BLOCKED`, exit 3) on an unoptimized build, on
binaries that the named build did not produce, while a compiler or another
receiver or display is running, or when the phone is not on `--port`. It
then opens Google Maps on the phone with a `geo:` intent and launches
`aa-wired-receiver` and `aa-dummy-display` (on the owner's session display).
While the window runs it drags the map once a second over the display's test
control channel. Media is required (`--media-uri`): the A/V contract needs
audio across the whole drive.

**Media for the whole drive.** When no audio write has arrived for
`MEDIA_RELAUNCH_SILENCE_S` (5 s) during the drive, the runner re-sends the
media intent and then the Maps intent, so Maps stays in front. It does this at
most once per 5 s and counts the relaunches in `media_relaunches`. A
relaunch was chosen over the alternatives because it is the only one that
works with any player and any clip using what the runner already has: VIEW
intents carry no portable loop option (VLC and Samsung's player differ), and
the host cannot check a clip's length on the phone without extra tooling. A
clip that ends is restarted. (In the S24 run of 2026-11-16, audio telemetry
stopped at 243 s; why is unproven.) The coverage clause still judges what
actually played, so restart gaps count against it.

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
| `AV_MEAN_WINDOW_S` = 5, `AV_MEAN_MAX_ABS_MS` = 40 | (a) the signed mean skew of every full 5 s window in the drive is within ±40 ms |
| `AV_P99_ABS_MAX_MS` = 50 | (b) the nearest-rank 99th percentile of \|skew\| is <= 50 ms |
| `AV_MAX_ABS_MS` = 90 | (c) the maximum \|skew\| is <= 90 ms |
| `AV_COVERAGE_MIN` = 0.90 | audio covers >= 90 % of the drive; no audio, or too little, is a FAIL, never `n/a` |

### Signed A/V contract (owner decision 2026-11-16)

This replaces the drive smoke's former "max \|skew\| <= 50 ms where pairs
exist" rule (#71 asked for an owner-approved A/V qualification contract). All
three clauses and coverage must pass.

- **Skew** is signed: (video `completed_ns` − video `scheduled_ns`) −
  (audio `presented_ns` − audio `scheduled_ns`), in ms. Positive means the
  video endpoint is later than the audio endpoint. A frame pairs with the audio
  slot whose half-open scheduled interval `[scheduled_ns, scheduled_end_ns)`
  holds the frame's `scheduled_ns`. The population is every painted frame whose
  paint completed between `active` and first paint + duration. A slot without
  an integer `presented_ns`, or with an empty interval, is skipped and counted
  in `invalid_audio_slots`. That differs from the perf collector, which
  invalidates the whole A/V metric for such a slot.
- **5 s mean (a):** for every full half-open window `[t, t + 5 s)` inside the
  drive `[first paint, first paint + duration]`, the sample mean of the skews of
  pairs whose paint completed inside it: a pair exactly at `t` is inside, one
  exactly at `t + 5 s` is not, and only full windows inside the drive count.
  The sweep evaluates the drive start and every change point, where a pair
  enters (`t = p − 5 s + 1 ns`) or leaves (`t = p + 1 ns`), so every distinct
  window is visited. A window with a single pair has that pair's skew as its
  mean. Windows without pairs have no mean; missing audio is judged by
  coverage.
- **Coverage:** the duration of the union of the valid audio slots' scheduled
  intervals, intersected with the drive `[first paint, first paint +
  duration]`, divided by the drive length. It shows that valid audio telemetry
  existed, not that a speaker played.
- **Result fields** (`av_signed_contract` criterion): `signed_p1_ms`,
  `signed_p50_ms`, `signed_p99_ms`, `signed_min_ms`, `signed_max_ms`,
  `abs_p99_ms`, `abs_max_ms`, `window_mean_min_ms`, `window_mean_max_ms`,
  `worst_window_mean_ms` (the larger in magnitude), `over_50_ms`, `pairs`,
  `invalid_audio_slots`, `coverage` and the per-clause `clauses`.

Rationale for each limit. These are engineering policy: the standards below
are about sustained, constant offsets between light and sound, and none of
them prescribes a p99, a rolling mean or a coverage rule.

- **±40 ms sustained:** EBU R37-2007 limits the end-to-end emission output to
  40 ms sound before picture and 60 ms after (and 5 ms / 15 ms per stage). The
  tighter side is used both ways until the physical offset is known.
- **p99 <= 50 ms:** keeps the previous contract's number, now as a tail
  statistic; 50 ms is 1.5 frame periods at 30 fps. A maximum has no stable pass
  probability over long runs: #71 records runs failed by 1 frame in 54,200,
  72,277 and 215,896 pairs.
- **max <= 90 ms:** ITU-R BT.1359-1 (1998) gives approximate average
  thresholds of 45 ms (sound early) / 125 ms (sound late) for detectability and
  90 ms / 185 ms for acceptability. 90 ms is its sound-early acceptability
  bound. Known real regressions (unoptimized builds at 106.8–173.3 ms, the
  30 ms display pump at 135.4 ms) exceed it.
- **Coverage >= 90 %:** without it, the easiest way to pass is for the media
  not to play; the S24 run of 2026-11-16 was judged on 33.8 % of its drive.
- **Internal-metric caveat:** both endpoints are software stamps. Video stops
  at the end of the Qt paint (`ProjectionWidget`), before the X server,
  scan-out and panel. Audio is the sink's playback slot (`presented_ns`), not
  speaker output. The physical offset, and even its sign, are unmeasured until
  #58. Keep the bounds symmetric until then; #58 may justify an asymmetric
  window.

Re-analysis of the 2026-11-16 evidence under this contract:

| Run | Pairs | Coverage | Signed p50 / p99 ms | Max \|skew\| ms | Worst 5 s mean ms | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| S24 drive smoke | 5,892 | 33.83 % (FAIL) | 23.21 / 51.02 (FAIL) | 74.95 | 32.61 | FAIL |
| A26 run2 | 17,895 | 99.49 % | 21.40 / 33.35 | 43.21 | 26.34 | PASS |

The same contract judges the soak stages from result version 3 on (see **Soak
stages: moving-map workload and signed A/V (v3)**). The math and the limits
live once, in `tools/perf/aa_av_contract.py`, for both.

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
- **the same phone**: the stage's `--phone-serial` (or `AA_PHONE_SERIAL`)
  must be attached on `--port` of the target (`adb devices -l` shows it as
  `device` with `usb:<port>`), and must equal the record's `phone_serial`.
  Soaks run on a test phone, normally the A26, so a smoke from another phone
  (including the owner's) never unlocks them. That adb check is only a fast
  pre-check: a phone swapped in after it would otherwise be measured. **The
  identity is bound where the port is used.** The launcher hands the verified
  serial to the on-target collector, which starts the receiver with
  `--require-usb-serial`. The receiver checks the admitted link's pre-switch USB
  serial (on Android, the adb serial) at the first link and at every reconnect,
  before the link carries a session. Another phone makes it log `err` code
  `phone_serial_mismatch` and stop; the collector then emits no capture, and
  the stage is `BLOCKED phone_identity_mismatch`. A mid-stage reconnect from a
  different phone was already refused by the receiver's same-key rule; it now
  gets this named reason too;
- **the default configuration**: the record's `configuration` (flags beyond
  the harness's own sockets, port, store, lifetime and control channel, plus
  the receiver and display decoder and the presentation mode) must be the
  default, which is what every soak stage launches;
- the default video offer and the full 600 s duration;
- `finished_utc` within `RECORD_MAX_AGE_S` = 24 h **by the real clock**. The
  launcher's `--now` only dates the result document and is never used for
  this check; `aa_drive_smoke.py check --now` exists for tests and offline
  review only. The 1800+7200 s chain takes about 2.5 h, so this is a
  same-day rule that an overnight phone or Maps update cannot slip past.

The record is strict JSON (`NaN` and `Infinity` are refused), and every field
the gate reads must be present with the right type (`video_profiles` must be
present and `null`; `phone_serial` and all `configuration` keys must be
present). The record schema is `aa-drive-smoke/2`: records written before the
signed A/V contract are refused as `drive_smoke_missing`. Both binaries must hash on the target
to a 64-hex-digit sha256. A display that cannot be hashed, or a stage with no
display, is blocked.

Otherwise the stage is `BLOCKED` with `drive_smoke_missing`,
`drive_smoke_malformed`, `drive_smoke_hash_failed`, `drive_smoke_not_passed`,
`drive_smoke_build_mismatch`, `drive_smoke_phone_unverified`,
`drive_smoke_phone_mismatch`, `drive_smoke_configuration`,
`drive_smoke_profile`, `drive_smoke_short` or `drive_smoke_stale`; and a stage
whose receiver found another phone on the port is `BLOCKED
phone_identity_mismatch`. Offline `--capture` replay and
`--readiness-only` do not need a record.

Gate budget (one gate out for each gate in): the **60 s soak stage is
retired** (owner decision 2026-11-16) as the gate out for #137. It was a
one-minute shakedown of the same static workload that the 1800 s stage then
repeats. The drive smoke now runs first, on the same build, for ten minutes,
with a moving workload. The soak chain is the 1800 s stage, then the 7200 s
stage.

## Soak stages: moving-map workload and signed A/V (v3)

From capture `aa-projection-perf-capture/3` and result `aa-projection-perf/3`
on (#137), every live soak stage (1800 s, then 7200 s) runs the drive smoke's
workload and is judged by its A/V contract.

**Workload.** `tools/perf/aa_moving_map.py` is the one implementation, shared
with the drive smoke. Before the receiver starts, the collector resets the
phone's USB mode, wakes it, starts the media and then Google Maps, so Maps is
in front. Every phone command is `adb -s <--phone-serial>` on the target. The
managed display is started with `--test-only-control-stdin`. From projection
`active` on, a stimulus thread drags the map once a second, and it re-sends
the media intent, then Maps, after `MEDIA_RELAUNCH_SILENCE_S` (5 s) without an
audio write, counting each relaunch in `workload.media_relaunches`. A display
that stops accepting control input for `CONTROL_WRITE_TIMEOUT_S` ends the stage
with terminal error `display_control_stalled`, and one whose control input
closes ends it with `display_control_closed` (both FAIL: the display is part of
the product under test). Every live stage needs `--media-uri` and
`--media-component PKG/ACTIVITY` (the player the cleanup stops and verifies);
`--media-type` and `--geo-uri` are optional.

**Phone restore order.** Every cleanup path restores the phone the same way:
the normal end, an acknowledged stop, the forced fallback, the drive smoke and
the soak.

1. The intent gate is closed, so no media or Maps intent can follow.
2. The media package (from `--media-component`) is force-stopped, and
   `pidof` must then show no process. A failed command or a player that is
   still running is retried, up to `MEDIA_STOP_ATTEMPTS` (3) times, 1 s
   apart. The whole restore stays within `RESTORE_BUDGET_S` (90 s): an
   attempt starts only while it (two adb commands of `ADB_TIMEOUT_S` = 20 s)
   and the USB switch still fit. Otherwise the restore goes straight to the
   USB switch and records the media stop as not done.
3. Only then is the USB mode reset (`svc usb setFunctions mtp`). That switch
   re-enumerates USB and breaks adb for a moment, so a command sent right
   after it is lost.

The 2026-11-17 A26 run hit exactly this. The old order switched USB first,
the force-stop that followed got "device not found", and VLC stayed alive
(paused).

A step that fails or cannot be verified is a named entry in
`workload.cleanup_failures`, in the capture and in the v3 result (the drive
smoke's result has the same field):

- `media_stop_unverified: <pkg> after 3 attempts: <why>`;
- `media_stop_unavailable` (no media package; live runs refuse this case
  up front, below);
- `usb_reset_failed: <why>`;
- `phone_restore_not_reached` (the collector ended before its restore).

**A failed restore fails closed.** `--media-component PKG/ACTIVITY` is required
for every live moving-map run: the launcher, the chain, the collector and the
drive smoke all refuse a missing component, or one without a package, before
the phone is touched. The restore stops and verifies exactly that package.

Any cleanup failure makes a live v3 stage that would otherwise pass
**BLOCKED** by name, so the chain never starts the next stage after it:

- `cleanup:media_not_stopped`;
- `cleanup:usb_not_restored`;
- `cleanup:phone_restore_not_reached`;
- `cleanup:not_recorded` (a capture without the record).

A measured budget miss stays a FAIL. The drive smoke's result is BLOCKED the
same way (exit 3), so the soak gate refuses it. The qualification checker
requires `workload.cleanup_failures` to be `[]`.

Every phone command goes through one lock with a closed gate. The phone
restore waits for an in-flight command, closes the gate and restores, so no
intent (in particular no Maps relaunch) can reach the phone after its restore,
whatever the stimulus thread is doing. A relaunch also checks the stop flag
between its media and Maps intents, and the collector stops and joins the
stimulus before it restores the phone.

**Phone commands are checked.** Every required phone command (the USB reset,
the wake key, the media and Maps launches, and each relaunch) must exit 0
within the 20 s adb bound, and an `am start` must not report `Error` (it can
exit 0 for an activity it did not start). The first failure ends the workload:
nothing more is sent to the phone but the restore.

- During the setup, before the receiver starts, the collector refuses the
  stage (`error: phone_setup_failed: <step>: <why>`) and the launcher reports
  **BLOCKED** `phone_setup_failed`.
- Later (a failed relaunch), the capture records `workload.phone_failure` and
  the reporter BLOCKS the stage with `run_evidence:moving_map_phone`. The stage
  still runs to its end: cutting it short would turn the harness failure into
  an `incomplete_active_window` FAIL.

BLOCKED, not FAIL, because adb and the phone's apps are the test harness, not
the receiver or display under test: a failed intent means the required
workload was not set up, the same evidence gap as the other `run_evidence:*`
rules. The drive smoke applies the same rule (BLOCKED `phone_command_failed`).

**Workload evidence.** A live v3 stage must prove the workload ran, both in
the reporter and in the qualification checker. The record must be
`kind: moving-map` with `stimulus_started: true`, and it must have at least
`MIN_DRAG_RATE_HZ` (0.25) completed drags per active second. The nominal rate
is about 0.65 Hz (one drag per second plus the drag's own ~0.5 s), so the floor
leaves room for adb relaunches.

The aggregate alone would accept a run that drags early and then stops, so the
drags must also be continuous. The capture records every completed drag's time
(`workload.drag_t_ns`), and no stretch of the active window, its edges
included, may go longer than `MAX_DRAG_GAP_S` (10 s) without one. The result
records the longest stretch as `workload.max_drag_gap_s`. Nominal spacing is
about 1.4 s. A media relaunch puts two `am start` calls (about 1-3 s each)
between two drags, so a healthy run stays under about 8 s; 10 s keeps that
margin and still catches a stimulus that stopped.

Media presence is already gated by the A/V coverage floor. Missing or
insufficient evidence is **BLOCKED**: `run_evidence:moving_map_workload`,
`_stimulus`, `_phone`, `_drags` or `_drag_gap`. It means the required workload
was not shown to have been measured, which is an evidence gap, like every
other `run_evidence:*` rule, and never a PASS. A stimulus whose display control
input stalled or closed is a FAIL. The v3 schema requires every workload field
(`media_relaunches`, `drags`, `stimulus_started`, `phone_failure`,
`max_drag_gap_s`) for a moving-map record.

**No build during a stage.** The collector scans the target's processes at
every sample for the compiler and build tools the chain's quiet gate waits on
(`BUILD_NAMES`; its own ancestors excluded). Every observation goes into
`concurrent_builds` in the capture and `run_status.concurrent_builds` in the
v3 result. Any observation makes a stage that would otherwise pass
**BLOCKED** `interference:concurrent_build`: the stage did not run on the quiet
host it claims. A capture without the watch is BLOCKED
`run_evidence:concurrent_build_watch`. As with every BLOCKED reason, a measured
budget miss is still a FAIL. The qualification checker requires
`run_status.concurrent_builds` to be `[]`.

**A/V (signed contract).** The collector keeps every pair's paint-completion
time and signed skew, and the merged union of valid audio slots' scheduled
intervals, in `av_series` (ns from the monotonic origin, frozen at the
measurement cutoff like every other measurement). The reporter judges them over
its own replayed active window with the four clauses above. They are the
gated metrics `av_window_mean_abs_ms` (<= 40), `av_p99_abs_ms` (<= 50),
`av_excursions` and `av_hard_cap_pairs` (the soak's maximum, below) and
`av_coverage_pct` (>= 90, `op: ge`), and the `av_contract` block carries the
signed distribution. `av_max_abs_ms` and `av_sync_ms` (the run-cumulative
maximum) are informational. Three rules differ from the drive smoke, on
purpose:

- **The maximum is a rate, not a cap** (owner decision 2026-11-17; below).

- **No audio is a FAIL** in a measured window: coverage is measured as 0 %.
  A run with no active window at all is still BLOCKED, as nothing was measured.
- **Invalid A/V telemetry still BLOCKS.** The soak keeps its stricter rule: a
  null `presented_ns`, mixed endpoint contracts or a malformed endpoint
  invalidates A/V for the stage (the drive smoke instead skips such a slot).

Everything else in the soak contract is unchanged: RSS, CPU, RSS growth, audio
latency, crashes, coverage, continuity, provenance and the build guard.

**Soak A/V maximum: an excursion rate (owner decision 2026-11-17).** For v3
soak stages, "max |skew| <= 90 ms" is replaced by:

- An *excursion* is an A/V pair with |skew| > `AV_SOAK_EXCURSION_MS` (90 ms).
  Every pair counts, so consecutive pairs of one spike count separately.
- A stage may have at most floor(active window s /
  `AV_SOAK_EXCURSION_PERIOD_S`) excursions (`AV_SOAK_EXCURSIONS_PER_S` =
  1/1800): **1** in the 1800 s stage, **4** in the 7200 s stage.
- Hard cap: no pair may exceed `AV_SOAK_HARD_CAP_MS` (150 ms).
- Unchanged: the 5 s window mean (±40 ms), p99 (<= 50 ms) and coverage
  (>= 90 %).

The constants are in `tools/perf/aa_av_contract.py`. The v3 schema pins them
in `budgets` and `av_contract.limits`. `aa_av_contract.evaluate(...,
max_clause="soak_rate")` judges the stage, and the same math with
`max_clause="cap"` judges the drive smoke. The result reports:

- the `av_excursions` count against its allowance (`budget_max`);
- `av_hard_cap_pairs` (budget 0);
- every excursion's offset into the active window and its signed value
  (`av_contract.excursion_pairs`; at most 1000 are listed, and
  `excursion_pairs_truncated` says when there are more, while the count is
  always exact);
- the max |skew| as an informational `av_max_abs_ms`.

The qualification checker requires the allowance to be the one the stage's
own active window gives.

Rationale:

- A maximum over a fixed limit grows with run length: the longer the stage,
  the likelier one stray pair crosses it, whatever the system's quality. A
  rate keeps one imperceptible frame from failing a long stage.
- The 150 ms cap still fails any single gross error. The 5 s mean, the p99
  and coverage still catch sustained offset, a heavy tail and missing audio.
- The first live 1800 s moving-map soak
  (`~/aa-research/pr152-live-evidence-2026-11-17`) had exactly one pair over
  90 ms: 90.577 ms, 385.9 s into the window, with p99 31.3 ms and coverage
  99.97 %. Under the cap it FAILed. Re-analysed under this rule, the same
  capture has 1 excursion of an allowance of 1, none over 150 ms, and PASSes
  (`~/aa-research/pr152-av-rate-reanalysis-2026-11-17`).

The 10-minute drive smoke keeps the plain max |skew| <= 90 ms: a single pair
at 90.1 ms still FAILs it. v2 history is unchanged.

**Conflicts with the new workload.** None found in the gates themselves. Two
operational effects to expect:

- A media relaunch restarts the clip. The collector's 10 s stream-silence rule
  (`_MEDIA_STALL_SECONDS`) is unchanged, and a relaunch fires after 5 s of
  silence, so a player that stops is restarted well before a stall. A player
  that takes longer than about 5 s to resume would still be judged a stall;
  this is intended.
- The control channel adds per-touch `ok` lines to the display log, and drags
  add GUI-thread work on the dummy display during every stage. Both are part
  of the measured candidate, as in the drive smoke.

**Migration and history.** The version is bumped explicitly; nothing is
reinterpreted:

- A capture `/2` replays to a result `/2` under `aa_perf_report.BUDGETS_V2`
  (A/V max <= 50 ms, gated), byte for byte what the origin/main reporter gives
  (checked on the 2026-11-15 1800 s PASS, FAIL and BLOCKED captures).
- `aa-projection-perf-v2.schema.json` validates v2 results unchanged;
  `aa-projection-perf-v3.schema.json` validates v3; `aa_perf_schema.py
  validate-auto` picks the schema from the document's own `schema`.
- The qualification checker (`tools/qualification/results.py`) judges each
  result by its own version: v2 by the v2 gates, v3 by the v3 gates plus a
  passed `av_contract` that agrees with the metrics, coverage >= 90 % and the
  moving-map workload. A package may hold either version per result.
- Live stages from this version on produce v3 only.
- One limitation of history, unchanged by this: results written before the
  `build` block existed (for example the 2026-11-15 `av-outlier` 1800 s PASS)
  do not validate against the current v2 schema. Their captures still replay to
  their original verdict.

**The chain.** `tools/perf/aa_soak_chain.py` replaces the ad-hoc chain scripts
kept beside past runs. It waits for `--quiet-seconds` (default 300) without a
compiler, then runs the 1800 s and 7200 s stages through
`aa-projection-perf.sh` with the moving-map workload, `--phone-serial` and
`--drive-smoke`. It stops at the first stage that does not PASS and writes
`stage-<N>s-not-run.json` for the rest.

The chain also checks for builds itself, every 5 s while a stage runs
(`stage-<N>s-build-watch.jsonl`). If any are seen, it writes
`stage-<N>s-interference.json` with every observation. A stage that would PASS
then counts as BLOCKED (exit 3), and its result is moved to
`stage-<N>s-result.disqualified.json`, so no qualifying file remains. A FAIL
stays a FAIL. The chain stops there. The stage is not cut short: the collector
records the same builds and its result is BLOCKED with the evidence, while
killing the stage would lose that named result.

**Host preflight (#154).** Before each stage the chain checks two host
conditions that stalled the PipeWire Pulse server for ~400 ms in an 1800 s
stage and so made its audio unqualifiable. It does not change a threshold; a
host that fails a check cannot qualify audio, so the stage is not run. The
code is `tools/perf/aa_host_preflight.py`; every check is read-only. Every
command goes over the launcher's own ssh transport and options to `--host`,
also for `localhost`, so the uid and the PipeWire session checked are the ones
the soak runs as, even when an ssh config maps the host to another `User`.

The checks run in this order, so the ones a launch depends on are the
freshest. After the timers, package maintenance is read once more, without
waiting (work that began meanwhile BLOCKs: `... (at the final check)`), and
then the timers again, last, on a fresh host clock. An ssh failure of any
gating probe BLOCKs as `host_transport:...` with ssh's own message:

| Check | Reads | Result |
| --- | --- | --- |
| Package maintenance | processes `apt`, `apt-get`, `dpkg`, `unattended-upgr(ades)`, `apt.systemd.dai(ly)`; `systemctl is-active apt-daily.service apt-daily-upgrade.service`; PackageKit over `busctl --auto-start=no` | Transient, so the chain **waits** for it, polling within the preflight's deadline (below); still busy is BLOCKED. PackageKit is busy only with a transaction (`GetTransactionList`) or the package lock (`Locked`): an idle resident `packagekitd` does not block. The bus name is checked with `NameHasOwner` and the daemon then addressed by its validated unique name (`:N.M`), so the query never D-Bus-activates PackageKit; a reply without its exact shape is BLOCKED. |
| PipeWire real-time | `ps -L -o cls,rtprio,comm -p PID` for every `pipewire` and `pipewire-pulse` process of the soak user | Every `pw-data-loop` thread must be `RR` or `FF`. A process that is not running, has no data-loop thread, or has any other class (`TS` is SCHED_OTHER) is BLOCKED. |
| PipeWire ERR counters (informational) | `pw-top -b -n 2`, only when a `pipewire` process of the soak user already runs; then the pids again | Never gated. Skipped (`pipewire_not_running`) otherwise. Residual race (accepted): PipeWire may exit between the guard and `pw-top`, which can then socket-activate the user's own audio server; the pids no longer match the ones judged real-time, so the stage is BLOCKED (`host_audio:pipewire_changed_during_preflight`), never OK on a daemon the preflight started. |
| apt timers | `systemctl show -p NextElapseUSecRealtime -p NextElapseUSecMonotonic` for `apt-daily.timer` and `apt-daily-upgrade.timer`; the host's monotonic and realtime clocks | The earlier of the calendar and the monotonic (`OnBootSec=`, `OnUnitActiveSec=`, ...) elapse, at or before the host's now (read after the queries) + duration + `--timer-margin-s` (default 900 s: the launcher's setup and cleanup), is **refused**: BLOCKED, with the earliest safe start in the reason. It does not wait, because the wait could be hours. An overdue timer counts as inside. A missing property, an unparsable, impossible or out-of-range (over a century) value, or an unreadable host clock is BLOCKED; an empty, `n/a` or `0` value means no next elapse. |
| Package maintenance (final), then apt timers (final) | as those rows, once, no wait; the timers from a fresh host clock | Any work, or a timer now inside the window, BLOCKs the launch. |

**One deadline.** A positive `--maintenance-wait-s` (default 900 s) bounds
the whole preflight, from its first command (`id -u`) to the verdict, on a
monotonic clock. The guarantee is the last step: when the elapsed time is over
the bound, the stage is BLOCKED (`host_preflight:preflight_deadline_exceeded`),
whatever the checks saw. Each command's budget (ssh included) is also capped to
the time left, and a maintenance poll that completes late is rejected
(`host_maintenance:wait_limit_exceeded`), only to fail early. 0 means a single
maintenance observation and no wait; every command then has its own bound.

The chain's build check is repeated right after the preflight, immediately
before the launch: the only gap left is that `/proc` scan and the launcher's
start, and the stage's own build watch samples from its first second.
`--maintenance-wait-s` and `--timer-margin-s` must be finite and >= 0.

A check that cannot be made (command failed), or a preflight that raises, is
BLOCKED, not skipped. A BLOCKED stage exits 3, writes
`stage-<N>s-not-run.json` naming every reason (`host_audio:...`,
`host_maintenance:...`, `host_transport:...`, `host_preflight:error`,
`host_preflight:invalid_record`, `interference:...`). The sidecar is strict
JSON: a record that would hold NaN or Infinity is replaced by a BLOCKED one. and the
later stages get theirs. The host is expected to keep `zucchi` in the
`pipewire` group (limits.d grants rtprio 95) and to pin the timers outside the
stage window, as the owner has
(`/etc/systemd/system/apt-daily*.timer.d/zz-aa-soak-window.conf`, 04:00 and
04:30 daily, no random delay); the preflight only notices when that comes
undone, it changes nothing.

Each stage's record is `stage-<N>s-host-preflight.json` (schema
`aa-host-preflight/2`): the maintenance state and how long the chain waited,
the scheduling rows, both timer elapses and the window used, and the verdict.
It is a sidecar like `stage-<N>s-interference.json` because the capture is
sha256-bound into the result and must not be edited. It also carries,
informationally and never gated, PipeWire ERR counters from `pw-top -b -n 2`
(the receiver's streams, the `*_output.*` sinks and the nodes):
`pipewire_err_counters.start` (before the launch) and `.end` (after the stage)
are host-node snapshots only, because the receiver's stream does not exist
then; `.during` is sampled while the stage runs (first 120 s after the launch,
then every 300 s) and keeps the first and latest sample that saw the
receiver's stream, so compare those two for the stream and `start`/`end` for
the sinks. Every snapshot is taken only from a PipeWire that is already
running. On a stop the chain TERMs the stage first and kills an in-flight
sample's ssh; it never waits for a sample before forwarding the TERM. An
interrupted stage never leaves a qualifying result. The stop flag is sticky,
and the chain checks it at the one point where a stage's result is finalised
and again before `CHAIN COMPLETE`: a build the watch saw is disqualified first
(interference wins over PASS), a PASS still on disk is moved to
`stage-<N>s-result.interrupted.json` with an `interrupted` field, and the
chain exits 128+signal, never 0. `tools/qualification/check_wired.py` rejects
an `*.interrupted.*` or `*.disqualified.*` result, or one carrying
`interrupted`, even when the manifest references it (`RETIRED_RESULT`).
An interruption also wins the exit code over a build block or a
disqualification that happened too, including a stop that lands while the
block markers are being written: the chain re-checks the flag after writing
any marker, and the stage's `not-run` marker names both reasons. No launcher
is started once a stop is pending (checked at the launch boundary and again
right before the spawn). A stop that lands during the spawn itself is seen
as soon as `Popen` returns: the chain stops the new launcher's group at once
(TERM, the grace, then KILL), starts no sampler or watch, and exits
128+signal. With a stop pending, the chain does not start the end probe, so
an interrupted stage normally has no `pipewire_err_counters.end`. A stop
that lands during that probe can leave an `.end`; it is then marked
`"interrupted": "<SIGNAL>"` and is not a clean after-stage snapshot. In
either case read `.during.latest` / `.during.latest_with_receiver` (when the
sampler ran) and the stage's `-logs/` instead. Only
the chain runs the preflight; a direct `aa-projection-perf.sh` invocation
does not, so check the host by hand before one.

The chain handles SIGTERM/SIGINT with a stop flag, as the drive smoke does.
Each stage runs in its own local process group: the launcher and its local
helpers. On a stop, the group gets TERM. The grace, `STOP_GRACE_S` (180 s),
applies to the whole group: the chain polls the group until it is empty, even
after the launcher itself has exited, and KILLs only what is left when the
grace expires. The interrupted stage and every later one get a not-run marker,
and the chain exits 128+signal. A stop during the idle gate starts no stage.

**The stop reaches the target.** The collector runs on the far side of the
launcher's ssh session. Ending a non-PTY ssh client does not signal the remote
command, so the local group alone could leave the collector, receiver, display
and stimulus running, and the phone unrestored. The launcher therefore makes
the stop explicit:

1. It starts the collector with `--run-file /tmp/aa-perf-run-<random>`. The
   collector creates that file exclusively (mode 0600). It records its own
   PID and those of the receiver and display, each with its `/proc` start
   time, plus the phone serial and media package. It also makes itself the
   leader of its own process group and records it (`group PGID SID`). Its adb
   commands are in that group; the ssh session's shell is not.
2. On TERM/INT, the launcher runs `tools/perf/aa_remote_stop.sh` on the
   target over a separate ssh call.
3. The script TERMs the collector, which runs its normal cleanup: stimulus,
   display, receiver, then the phone restore. Only then does the collector
   remove the run file. That removal, within `AA_REMOTE_STOP_ACK_S` (30 s), is
   the acknowledgement. Just before it, the collector writes
   `<run file>.cleanup`, with one line per restore step it could not do or
   verify. The script relays it:
   - `remote-stop: acknowledged` only for an empty report;
   - otherwise, or with no report at all, `remote-stop: failed: acknowledged
     with cleanup failures: ...` and exit 1.
4. Without an acknowledgement (a hung cleanup, or a collector already gone),
   the script falls back independently:
   - It KILLs the collector's whole process group, so an in-flight adb command
     dies with it, then the receiver's and display's groups.
   - A group is signalled only while it is still the recorded one: the same
     PGID and session and, if a live process leads it, the recorded leader by
     start time. A reused PID is never signalled.
   - It waits until every killed group is empty, then restores the phone
     itself in the same order: force-stop the media player and verify it with
     `pidof` (up to 3 attempts), then `svc usb setFunctions mtp`. It prints
     `remote-stop: forced`. Nothing the collector started can send an intent
     after that restore.
   - If a process survives the KILL, or a restore step fails or cannot be
     verified, the script still attempts the restore but prints
     `remote-stop: failed: forced: ...` naming each problem and exits 1. The
     launcher reports that the remote stop reported a failure.
5. The launcher then drops its own ssh client and exits 143 (130 for INT).

**Every collector exit is confirmed on the target.** The collector's ssh bound
is the sum of three named budgets in the launcher, each justified there:

- `COLLECTOR_STARTUP_BUDGET_S` = 180: ssh connect, the four bounded adb setup
  commands and their waits (about 87 s), and the 60 s receiver/display
  startup bound;
- the requested window plus `COLLECTOR_MEASUREMENT_SLACK_S` = 30: one
  sampling interval of overshoot and the frozen-collector log grace;
- `COLLECTOR_CLEANUP_BUDGET_S` = 240: the stimulus join (up to 47 s), the
  display and receiver terminations (about 20 s), the media stop (3 attempts
  of two 20 s adb commands, about 123 s) and the USB switch (20 s).

However the collector ends (a normal exit, a refusal, a dropped link or that
bound expiring with exit 124), the launcher then runs `aa_remote_stop.sh`
with no wait for the run file to appear:

- A finished run's cleanup report is relayed and removed
  (`remote-stop: finished`, or `failed: finished with cleanup failures`).
- A collector that is still running, for example during a stalled adb
  cleanup, is stopped and its cleanup confirmed, or the script falls back to
  KILL plus a direct restore.

On a timeout the stage is BLOCKED `collector_timeout`, never
`collector_failed` or a PASS. When the target cannot be confirmed clean,
every BLOCKED reason gains `,remote_cleanup_unconfirmed`. A normal exit whose
target is unconfirmed is BLOCKED `remote_cleanup_unconfirmed`. A finished
run's own cleanup failures are in its capture, and the reporter blocks them
by name.

**The stop is bounded, and its bound is derived.** Every wait and retry in
`aa_remote_stop.sh` comes out of named budgets:

- the run file's appearance (15 s on a signal, 0 after an exit) and the
  acknowledgement (`AA_REMOTE_STOP_ACK_S`, 30 s);
- `GROUP_WAIT_S` (10 s) for all the KILLed groups together;
- `RESTORE_BUDGET_S` (60 s) for the media stop and the USB restore, with
  `ADB_COMMAND_S` (15 s) per adb command;
- `REPORT_RESERVE_S` (2 s).

The worst case is the sum of those, 117 s on a signal. The script prints it
with `--worst-case ACK APPEAR`, run locally. The launcher bounds the stop's
ssh at that plus `REMOTE_STOP_SSH_MARGIN_S` (15 s: the 10 s connect timeout
plus 5 s), so its limit is strictly larger. The chain's 180 s grace covers the
132 s.

A media-stop attempt starts only while it and the USB restore fit in what is
left of `RESTORE_BUDGET_S`. Otherwise the script goes straight to the USB
restore and reports the media stop as not done (`media_stop_unverified: ...
restore budget exhausted`, which is `cleanup:media_not_stopped`). So even
with every adb call stalling to its bound, USB is restored and the outcome
reported inside the limit.

An explicit
stop with an acknowledgement is used rather than tying the collector to the
ssh session (SIGHUP or stdin EOF): the collector reads its own source from
stdin, a non-PTY session sends no SIGHUP, and a dropped link would look the
same as a stop. The explicit call also confirms the cleanup and has a fallback
when the cleanup does not finish.

Running the chain. **Prerequisite: an authenticated ssh target.** The
launcher, and so the host preflight, reaches the soak host only over
`ssh -o BatchMode=yes`, also when the chain runs on the Jetson itself. Pass
`--host` explicitly as a key-authenticated target whose host key is already
trusted, e.g. `--host zucchi@127.0.0.1` on the Jetson (the recorded Jetson
chains used exactly that); check it first with
`ssh -o BatchMode=yes "$SOAK_SSH_HOST" id -u`. The default `localhost` works
only when that is set up for `localhost` too. When ssh fails, the stage is
BLOCKED with `host_transport:ssh_failed (<ssh's message>)` or
`host_transport:unreachable`, not run.

```bash
SOAK_SSH_HOST=zucchi@127.0.0.1   # on the Jetson; else the Jetson's ssh target
tools/perf/aa_soak_chain.py --out "$RUN_DIR/chain" --host "$SOAK_SSH_HOST" \
    --phone-serial "$SOAK_PHONE_SERIAL" --drive-smoke "$DRIVE_SMOKE_RESULT" \
    --media-uri file:///sdcard/Music/<tone>.opus --media-type audio/ogg \
    --media-component org.videolan.vlc/org.videolan.vlc.gui.video.VideoPlayerActivity \
    --receiver "$AA_BUILD_DIR/aa-wired-receiver" --display "$AA_BUILD_DIR/aa-dummy-display" \
    --build-dir "$AA_BUILD_DIR" --store "$STORE" --thermal-policy orin-nano-cv-unavailable/1 \
    --provenance "$RUN_DIR/provenance.json" --display-env ":0,/run/user/$(id -u)/gdm/Xauthority"
```

## Exact invocations

From the development checkout (bounded ssh, fail-closed):

```bash
# SOAK_MEDIA_COMPONENT is PKG/ACTIVITY playing $SOAK_MEDIA_URI (required: the
# cleanup stops and verifies this player), e.g.
# org.videolan.vlc/org.videolan.vlc.gui.video.VideoPlayerActivity.

# Bound, read-only prerequisite check (safe to run anytime).
tools/perf/aa-projection-perf.sh --readiness-only --out /tmp/task-32-readiness.json

# 30-minute performance run (optimized build, see Build).
tools/perf/aa-projection-perf.sh --mode wired --duration 30m \
    --build-dir "$AA_BUILD_DIR" --drive-smoke "$DRIVE_SMOKE_RESULT" \
    --phone-serial "$SOAK_PHONE_SERIAL" --media-uri "$SOAK_MEDIA_URI" \
    --media-component "$SOAK_MEDIA_COMPONENT" \
    --out /tmp/task-32-perf-30m.json --capture-out /tmp/task-32-capture-30m.json

# Separate 2-hour soak run (fresh invocation; do not reuse the 30m capture).
tools/perf/aa-projection-perf.sh --mode wired --duration 2h \
    --build-dir "$AA_BUILD_DIR" --drive-smoke "$DRIVE_SMOKE_RESULT" \
    --phone-serial "$SOAK_PHONE_SERIAL" --media-uri "$SOAK_MEDIA_URI" \
    --media-component "$SOAK_MEDIA_COMPONENT" \
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
`tools/perf/aa-projection-perf-v3.schema.json`; historical v2 results against
`-v2`) whose gated metrics are compared to the contractual budgets:

- receiver RSS peak <= 250 MB; receiver CPU sustained <= 50 % of one core;
- RSS growth < 10 %; audio latency <= 200 ms; zero crashes;
- the signed A/V contract: every 5 s mean within ±40 ms, p99 \|skew\| <= 50 ms,
  at most floor(active s / 1800) pairs over 90 ms and none over 150 ms
  (owner decision 2026-11-17), audio coverage >= 90 % (v2: A/V sync <= 50 ms);
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

Both runs require capture `aa-projection-perf-capture/3` and result
`aa-projection-perf/3` (historical runs: `/2`, judged by the v2 contract), with
`measurement_coverage` policy
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
The drive-smoke record carries `phone_serial` so the soak gate can bind to the
phone; it is a local file and must not be committed or exported.
The collector records only target hardware state (machine, kernel, model, L4T
release, power mode, governor), not identifiers; raw captures stay under `/tmp`
on the host.
