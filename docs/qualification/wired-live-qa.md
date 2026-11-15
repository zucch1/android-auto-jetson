# Wired end-to-end live QA (task 30, phase B)

Phase-A (host-side, no hardware) acceptance for task 30 is `ctest -R wired_e2e`
plus the module verification recorded in
`.omo/evidence/jetson-android-auto-receiver/task-30-wired-e2e.json`. This document
is the **phase-B procedure**: it qualifies the real approved phone over USB/AOA on
`jetson.local` and drives the full projection path.

> Phase B is owner/orchestrator-controlled and was NOT run during phase A (the
> Jetson display is DP-1 DISCONNECTED). No projection claim is made until the
> live record below is produced.

## Build

Phase B must run an optimized build: `cmake --preset host-release` on the
Jetson, or `jetson-aarch64` for cross builds; `RelWithDebInfo` is acceptable
when symbols are needed. Never the Debug `host-dev` or `host-asan` preset: a
Debug build is not a slower measurement of the same code — building only the
projection widget's pixel conversion unoptimized reproduced a 106.8 ms A/V
failure against the 50 ms gate, where the same commit measured 16.2-30.1 ms
optimized, because that conversion runs on the GUI thread and delays paint and
the 5 ms display pump. Record the `CMAKE_BUILD_TYPE` and the effective C++
compiler flags (the build directory's `CMakeCache.txt`) in the run report.

The task-32 performance collector enforces this and refuses to start a stage
without that proof (`BLOCKED unoptimized_build`, see
`docs/qualification/wired-perf-runbook.md`); `--allow-unoptimized-build` there is
a tooling-debugging override, never a qualification run. This harness's own
`projection_timeout_ms` budget and every gate value are unchanged.

## Components wired on the target

| Stage | Module | Evidence |
| --- | --- | --- |
| USB/AOA transport | task 28 (`aa-usb-aoa-probe`, `aa_transport_usb`) | AOA switch + framed round trip, port `1-2.1` |
| Session lifecycle | task 17 (`aa_session`) | lifecycle transitions to `active` |
| Channels | task 18 (`aa_channels`) | discovery + channel opens answered |
| Video channel → consumer | task 22 (`aa_video`) + task 23/24 | video access units reach the dummy display |
| Audio | task 25 (`aareceiver-audio`) | routed to the current HDMI/DP PipeWire sink |
| Input | task 26 (`aa_input_service`) | display input reaches the input channel |
| Trust | task 27 (`aa_trust`) | approved phone admitted; revoke fails closed |
| Diagnostics | task 19 (`aa_diagnostics`) | redacted session telemetry |

The integrator is `aa::app::WiredSession` (`include/aa/app/WiredSession.hpp`,
`src/app/WiredSession.cpp`), built as `aa_wired` (see `cmake/WiredE2e.cmake`). It
composes the modules above and reimplements none of them.

## Prerequisites (once per target)

1. Task-29 udev policy installed:
   `sudo -n bash tools/usb-policy/install.sh` (matched ids on any host port,
   #133).
2. Approved phone connected on any host USB port (phone mode `04e8:6860`
   before the AOA switch; `18d1:2d00`/`18d1:2d01` after). Pass that port to the
   receiver with `--port`; the qualified rig uses `1-2.1`.
3. Approved-phone trust store loaded (task 27) for the phone's wired identity.
   A serial-backed wired approval matches the phone on any port (#133); a
   port-only (serial-less) approval matches only the port it was approved on.
4. Display connected (DP-1 or HDMI-A-1 `connected`) and a PipeWire output sink
   present (`pactl list short sinks` or `pw-cli`). The perf harness
   (`tools/perf/aa-projection-perf.sh`) exports the X session it is told about
   with `--display-env DISPLAY[,XAUTHORITY]`; it falls back to the caller's
   exported `DISPLAY`/`XAUTHORITY` and only then to `:1`. X11 clients also need
   `XAUTHORITY` pointing at the authority cookie of the target X server (the
   owning session's cookie file) - without it the X connection is refused with
   an authentication failure. With GDM automatic login the owner's session is on
   display `:0` with its cookie at `/run/user/<uid>/gdm/Xauthority`, and `:1` is
   the display of a manually started second session: the qualification needs the
   display the owner's screen session is actually on, so pass it explicitly,
   e.g. `--display-env ":0,/run/user/<uid>/gdm/Xauthority"` (or export
   `AA_DISPLAY`/`AA_XAUTHORITY`, or that session's `DISPLAY`/`XAUTHORITY` for
   the run). Read `--help` and the runbook's display-session note; a value that
   is not a plain session path is a usage error.
5. A wired receiver binary on the target: `aa-wired-receiver`
   (`tools/wired-receiver/main.cpp`), which runs `aa::app::WiredSession` over
   the real `aa_transport_usb` endpoint with `transport::admit_via(TrustStore)`
   admission and serves the task-21 D-Bus contract, the task-22 video server,
   the task-24 input server and the task-25 audio router. CLI:
   `aa-wired-receiver --port <usb-port-context> [--store PATH] [--no-bus]`.
   The source is in this tree and builds with the host presets
   (`cmake --build --preset host-dev --target aa-wired-receiver`), but the
   target binary is NOT yet buildable from this tree: `aa_protocol_adapter`
   (the AASDK-backed protocol codec) is native-only - the frozen cross sysroot
   has no AASDK development inputs - and the aarch64 AASDK + libdbus enablement
   is todo 41 scope. Until then, the live projection step below is BLOCKED on
   `receiver_binary_unavailable_on_target` and is recorded as such.

## One documented command sequence

From the development checkout (bounded ssh, fail-closed):

```bash
tools/wired-qa/run-wired-live-qa.sh --readiness-only

tools/wired-qa/run-wired-live-qa.sh \
    --receiver /opt/aa/aa-wired-receiver \
    --display  /opt/aa/aa-dummy-display
```

Each invocation writes `result.json` with mode 0600 in a fresh private
`$XDG_RUNTIME_DIR/aa-wired-live-qa.XXXXXX` directory (mode 0700) and prints
the full result path. `XDG_RUNTIME_DIR` must name an existing directory.
An explicit `--out` requires an existing parent directory that is not
world-writable and a new file path; existing files and symlinks are refused.
Use a private directory for explicit output, never a path directly under `/tmp`.

The harness exits non-zero with a `BLOCKED` result (`blocked_on`) whenever a
prerequisite is unmet - an unreachable host, an absent phone, an uninstalled
policy, a disconnected display, a missing audio sink, or a projection that never
reaches the contract within the budget. Exit 0 requires the D-Bus `GetState`
contract to report `projecting` **and** the receiver/display logs to show video
and audio activity; a green smoke without those observables is never a pass.

## Observables to record (fed into the task-30 receipt)

- `projection_reached`: `busctl --user call org.custom.AndroidAutoReceiver
  /org/custom/AndroidAutoReceiver org.custom.AndroidAutoReceiver1 GetState`
  reports `projecting`. This is a consumer-admission signal ONLY - it does not
  prove phone media delivery, and `projecting` with no observed media is
  NON-PASS.
- `video_present`: BOTH the receiver log's `video_sent` events (units crossed
  the task-22 channel) AND painted frames on the dummy display (`frames_painted`
  > 0 with `render_ok` from the shipped test-only status channel,
  `tools/dummy-display/test_control.hpp`). `video_sent` alone only proves units
  crossed the channel, not that anything painted.
- `audio_present`: BOTH `audio_chunks` and `media_timing` records from the
  receiver log. The current receiver emits `audio_chunks` only after a
  successful pump writes frames; timing alone can also appear at final flush
  after a failed pump and is not delivery evidence.
- `input_evidence`: an `input_forwarded` record with `"ok":"true"`. The bare
  event is not proof of delivery (`"ok":"false"` means the send failed).
- `startup_budget_ms`: the receiver's `active` event value (session start ->
  active projection wall time), compared against the wired reconnect/startup
  budgets.

## Budget caveats carried from task 9 (do NOT claim)

Task 9 is reduced-scope (owner-signed): the decode p95 latency budget (<=33 ms at
1280x720@30) and the software H.264 fallback qualification are **withdrawn**.
Phase B must not claim them. A/V-sync and audio-latency budgets are measured in
task 32.

## Privacy

Device serials/MACs/IPs/host names are never written to committed content or
the receipt; the harness redacts before export (the JSON artifact carries the
host only as a truncated SHA-256 digest, and remote command arguments are
shell-quoted so caller-supplied values stay literal). Raw captures stay under
`/tmp` on the host.
