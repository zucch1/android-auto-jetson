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

The receiver/display pair supplies cross-stream completion events and audio
timing (`av_sync_ms`, `audio_latency_ms`, plus informational audio scheduling
skew). The following **7 of the
9 event-metric slots have no emitter** in that pair and stay `null` in every
capture it produces:

1. `frame_drops`
2. `decode_p50_ms`
3. `decode_p95_ms`
4. `decode_fps`
5. `decode_frames`
6. `ipc_throughput_mbps`
7. `ipc_p95_added_latency_ms`

The reporter fails closed on this: any absent event-metric slot is listed in the
result's `named_gaps`, `qualification_status` stays `INCOMPLETE`, and the verdict
is `BLOCKED` — such a capture is **never** presented as complete task-32
qualification and never a PASS. (Decode timing remains informational per the
task-9 caveat above and is not re-claimed here.) The emitter halves —
decode/IPC/frame-drop stats events — are cross-lane work in `tools/wired-receiver` /
`tools/dummy-display` (lanes 30/31) and land separately; until then phase-B
runs are expected to end BLOCKED on these named gaps rather than PASS.

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
