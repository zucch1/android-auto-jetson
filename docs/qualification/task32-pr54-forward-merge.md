# PR56 forward merge of the PR54 foundation

## Boundary and provenance

- Starting PR56 head: `13a235d7cac84516483a16d8616d3aebe78adecd` (clean).
- Immutable merged main: `8e23bda1c199e09483f69fe515bd0f7070eb1042`.
- Operation: local forward merge, without rewriting either history.
- Merge commit: `3408c595e11c84d7b5b20aebc10d17864bb42a73`, with the
  starting PR56 and immutable main commits above as its two parents.
- PR55/task31 is pending and is not imported. The integration reference
  `5dec44a` was not copied or merged. No later video telemetry, POLLHUP,
  or qualification features were imported from it.
- This is phase-A host tooling proof, not hardware or phase-B acceptance.
  No network, publication, hardware, configuration, or credential changes
  were performed for this reconciliation.

## Exact conflict resolutions

Git reported these 32 conflicted paths. Each was compared between the two
pinned parents before resolution; no global ours/theirs strategy was used.

| Conflicted path | Resolution |
| --- | --- |
| `CMakeLists.txt` | Keep main includes plus PR56 `WiredPerf.cmake`. |
| `cmake/ProtocolAdapter.cmake` | Main: focus, AV, microphone, sensor adapters and tests. |
| `cmake/UsbAoaTransport.cmake` | Main: isolated fake-libusb and expanded USB regressions. |
| `cmake/WiredE2e.cmake` | Main: owner-queue/shutdown tests, cipher wrapper, negotiated AV/control/microphone targets. |
| `include/aa/app/WiredSession.hpp` | Main: owned AV boundaries and channel/control APIs. |
| `include/aa/protocol/Messages.hpp` | Main: focus, sensor, microphone and in-band channel-open types. |
| `include/aa/transport/ByteEndpoint.hpp` | Main: owner-thread I/O checkpoints. |
| `include/aa/transport/Transport.hpp` | Main: owner-thread I/O checkpoints. |
| `include/aa/transport/UsbByteTransport.hpp` | Main: checkpoint forwarding contract. |
| `src/app/WiredSession.cpp` | Main: negotiated AV, lifecycle cleanup, deadlines and control dispatch; do not restore old raw routing. |
| `src/protocol/aasdk_adapter/ControlAdapter.cpp` | Main: service-channel partial-open compatibility with strict control decoder. |
| `src/transport/UsbByteTransport.cpp` | Main: cancellation/checkpoint handling around framed I/O. |
| `src/transport/usb_adapter/LibusbWorld.cpp` | Main: pre-switch no-claim, bounded accessory wait, pinned reset identity. |
| `src/transport/usb_adapter/UsbAoaEndpoint.cpp` | Main: checkpoint/deadline checks around transfers. |
| `src/transport/usb_adapter/UsbAoaEndpoint.hpp` | Main: checkpoint ownership. |
| `src/transport/usb_adapter/UsbAoaSwitch.cpp` | Main: wire string budget. |
| `src/transport/usb_adapter/UsbAoaSwitch.hpp` | Main: explicit 255-character/256-byte limit. |
| `tests/transport/usb_aoa_probe_test.cpp` | Main: UTF-8/hostile byte regressions. |
| `tests/transport/usb_aoa_switch_test.cpp` | Main: wire-budget boundary regressions. |
| `tests/usb/test_probe_binary.py` | Main: setup/watchdog/reset/hostile-argv cases. |
| `tests/usb/test_usb_policy.py` | Main: exact-policy, staging, trust and interruption cases. |
| `tests/wired_e2e/receiver_test.cpp` | Main: successful-send/drop accounting, actual negotiated PCM, owner queue and shutdown regressions. |
| `tests/wired_e2e/test_live_qa_gates.py` | Main: FIFO paint/render/input and successful-audio gates. |
| `tests/wired_e2e/wired_e2e_revoke_test.cpp` | Main: owner-thread revocation/cancellation/durability cases. |
| `tools/usb-aoa-probe/main.cpp` | Main: pinned reset, setup exit code and completion-aware watchdog. |
| `tools/usb-aoa-probe/probe_support.hpp` | Main: UTF-8-safe JSON output. |
| `tools/usb-policy/README.md` | Main: transactional state and honest ACL limitations. |
| `tools/usb-policy/install.sh` | Main: snapshot/exact-policy/trusted-state/atomic publication. |
| `tools/usb-policy/rollback.sh` | Main: transactional recovery and drift refusal. |
| `tools/usb-policy/rules_check.py` | Main: exact approved-policy verification. |
| `tools/wired-qa/run-wired-live-qa.sh` | Main: positive display fallback, paint/render evidence, audio pump evidence and actual FIFO input. |
| `tools/wired-receiver/main.cpp` | Main runtime plus PR56 timing adapter described below. |

The receiver retains main's `VideoOutlet` unchanged: sequence numbers count
attempts; `units_` counts only `SendStatus::sent`; no-consumer, waiting-IDR
and backpressure outcomes remain drops. Main's 48 kHz stereo media and
16 kHz mono guidance/system PCM profiles remain behind `register_av_sink`.
Microphone, sensors, input, owner-queue shutdown, admission and per-link
client-role cipher behavior remain main's implementation.

The timing adapter installs PR56's write-completion observer on those current
PCM profiles, emits audio-only `audio_scheduling_skew_ms` and cumulative
`audio_latency_ms` only after written frames, and invokes idempotent `finish()`
after joining the session thread, before router close. Destructor fallback
preserves early-return flushing. The collector, not audio skew, derives paired
`av_sync_ms` from audio write completion and Qt paint completion.

## Diff against immutable main

Before this receipt, the reconciled tree differs from `8e23bda1` in 29 paths:

This is the **main-parent comparison**, not the first-parent comparison.
Against starting PR56 `13a235d7`, the same tree has 86 changed paths: those
are the imported foundation changes plus the focused timing adaptation/tests.
The two counts intentionally differ because they compare different parents.
Including this receipt makes the final counts 30 versus main and 87 versus
starting PR56; it does not import another source branch.

- Performance tooling: `cmake/WiredPerf.cmake`, its root CMake include,
  `docs/qualification/wired-perf-runbook.md`, all six `tools/perf/` files,
  and the eight `tests/perf/` fixture/test files. Existing continuity/display
  checks, paired A/V diagnostics and fixed budgets are retained, not relaxed.
- Audio completion: `include/aa/audio/AudioRouter.hpp`,
  `src/audio/AudioRouter.cpp`, and the focused receiver timing adapter.
- Existing paint-completion telemetry: `ProjectionWidget.cpp/.hpp` and
  `tests/display/dummy_display_test.cpp`; no visual/layout redesign.
- Already-published certificate-chain support: `cmake/TlsCredentials.cmake`,
  `include/aa/tls/Credentials.hpp`, `src/tls/Credentials.cpp`,
  `src/transport/tls_adapter/TlsRecordCodec.cpp`,
  `tests/tls/credential_probe.cpp`, and `tests/tls/credentials.py`.

All other foundation paths match immutable main exactly. The only new test
delta beyond the two parents adds current-PCM profile timing, idempotent
finalization and empty-route coverage in `tests/perf/receiver_timing_test.cpp`.
This receipt is the additional documentation path.

## Focused proof and remaining gates

Host configuration: `cmake --preset host-dev`; all-target host build:
`cmake --build --preset host-dev` (exit 0). The first bounded build calls
were interrupted by shell time limits; resumed incremental build completed.
This reused the existing host-dev build directory, not a clean rebuild.
The final build log records the successful up-to-date check (`ninja: no work
to do`); earlier compilation output is in this execution's tool transcript.

Serial focused CTest runs, all passing:

- `^wired_perf_`: 15/15, including four `ReceiverTiming` GTests.
- `^dummy_display_`: 14/14, including real executable replay/flow tests.
- `^(wired_receiver|wired_deadline|wired_e2e_(av_sink|microphone|control|revoke)|audio_pipeline_.*)$`:
  11/11; includes negotiated AV/no setup PCM, send/drop accounting,
  owner-queue revocation/shutdown, deadline and audio paths.
- `^(wired_e2e_(auth|input_wire|happy|trust_link)|wired_e2e_live_qa_gates)$`:
  5/5 additional foundation smoke tests (`build/host-dev/task32-foundation-smoke.log`).

Local build-directory evidence: `build/host-dev/task32-{build,perf-tests,display-tests,wired-audio-tests,timing-tests}.log`.
`bash -n` passed for perf/live-QA/policy shell scripts. Receiver/timing-test
LSP diagnostics reported zero errors; the directory scan also reported zero
errors but is capped at 50 C++ files, so it is not an exhaustive LSP proof.
Additional uncapped directory checks for app, transport, protocol, channels,
Qt adapter, wired E2E and transport tests reported zero errors, as did direct
audio/router/header, session-header, TLS and USB-probe checks. No CMake LSP
is configured; configure/build provide that validation. Python syntax
compilation passed for perf and restored USB/live-QA files (cache confined
to the build directory). The actual receiver CLI rejected an invalid option
with usage exit code 2 (`build/host-dev/task32-cli-usage.log`).

The added timing tests are 119 nonblank/noncomment lines. The inherited
receiver entrypoint remains 660 such lines; this merge deliberately avoids
an unrelated entrypoint refactor. The new adapter owns timing export only,
adds no input parser, tagged variant, dependency or untyped escape hatch,
and reuses the existing callback and sink patterns.

The full nonlong suite was deliberately not run: task31's concurrent full
CTest can race global `/tmp` capacity fixtures. Run it later in an exclusive
window. PR55 recovery still needs its own final fix/gate before ordered
integration. No hardware soak, target build, acoustic output, physical scanout,
or end-to-end phone latency is qualified by this host result. Existing
completion endpoints are consumer write handoff and Qt paint completion,
not speaker output or physical display presentation.
