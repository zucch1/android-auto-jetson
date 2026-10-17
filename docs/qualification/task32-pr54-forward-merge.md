# PR56 forward merge of the PR54 foundation

The PR54 receipt below is historical. The final PR55 reconciliation and current
candidate gates are recorded in the final section; it supersedes the earlier
statements that PR55 is pending.

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

## Final PR55 source and squash forward reconciliation

### Pins, parents and tree preservation

- Clean starting local PR56: `8cdc13b2a7fe440b9acef4e97dcbb12556a8ef4e`.
  Published PR56 remains `13a235d7cac84516483a16d8616d3aebe78adecd`;
  this reconciliation was not published.
- Foundation: `8e23bda1c199e09483f69fe515bd0f7070eb1042`.
- Final PR55 source: `73d2a7ffc6ff643ace937b3ab9134a336e6ec986`.
- New immutable main squash: `0c607eae68a94f264158ebc564911fab5e0903ed`.
- Source and squash trees were both verified as
  `ba15e956bfe924aa0b4907d245406ae7a441e50e`, with an empty tree diff.
- Source-union merge: `3d7fa571a7595a3f6d9728f98cf9d4e923b85ba6`.
  Its ordered parents are the clean starting local PR56 and final PR55 source
  above. Its tree is `8099c4b16453b3d816774eea00c565db994f8955`.
- Squash-ancestry merge: `0b4fbcd34956b94a3dbfd0f82b01e048396878d5`.
  Its ordered parents are `3d7fa571a7595a3f6d9728f98cf9d4e923b85ba6`
  and `0c607eae68a94f264158ebc564911fab5e0903ed`.
  Its tree is also `8099c4b16453b3d816774eea00c565db994f8955`:
  the second merge has an empty first-parent diff, not a dependency rollback.
- Both source and squash are ancestors of the candidate; `5dec44a` is not.
  No integration-reference content, future telemetry or history rewrite was used.
  The subsequent receipt-only commit changes this document, not tested code.

### Individual conflict receipt

Source merge reported exactly four conflicted paths. Resolutions were applied
per hunk with no blanket ours/theirs selection:

| Path | Source-merge decision |
| --- | --- |
| `CMakeLists.txt` | Include PR55 `WiredRecovery.cmake` after the integrator, then retain PR56 `WiredPerf.cmake`. Both host test inventories remain enabled. |
| `tests/wired_e2e/test_live_qa_gates.py` | Retain source's expanded F3/F5/P2-G description. The auto-merged suite retains real FIFO/paint/audio/input evidence and the final-flush-without-audio rejection test. |
| `tools/wired-qa/run-wired-live-qa.sh` | Use source's exact-line DRM `grep -Fxq connected` check, not token splitting. Retain successful xrandr fallback and existing bounded process/FIFO evidence checks. |
| `tools/wired-receiver/main.cpp` | Keep PR56 `on_write_completed` observer on the current negotiated PCM configuration. Preserve PR55 rejected-attempt identity attribution and the existing negotiated sink routing. |

Squash merge reported exactly one conflicted path: `CMakeLists.txt`. Retain
the perf include alongside the already-present recovery include. Staging then
proved exact tree equality with the source-union parent before committing.

The automatically merged dependency paths retain final PR55 recovery accounting:
pending attempts resolve at active or terminal failure, admitted identity stays
distinct from rejected-attempt attribution, and USB admission remains fail-closed.
The link signature and receiver test linker wrapper move together. All dependency
paths other than the intentional CMake and receiver timing unions match source.

PR56 performance/audio/paint files are unchanged from starting `8cdc13b`.
`main.cpp` still has 48 kHz stereo media and 16 kHz mono guidance/system
profiles, `register_av_sinks`, successful-write-only cumulative timing,
`audio_scheduling_skew_ms` rather than mislabeled A/V sync, the completion
observer, and idempotent `finish()` after session-thread join before router
close (plus destructor fallback). Paired A/V sync still uses audio-write and
Qt-paint completion. Fixed budgets and continuity gates were not relaxed.

### Exact diff against new main

The candidate differs from `0c607ea` in these 30 paths, including this receipt:

```text
CMakeLists.txt
cmake/TlsCredentials.cmake
cmake/WiredPerf.cmake
docs/qualification/task32-pr54-forward-merge.md
docs/qualification/wired-perf-runbook.md
include/aa/audio/AudioRouter.hpp
include/aa/tls/Credentials.hpp
src/audio/AudioRouter.cpp
src/display/qt_adapter/ProjectionWidget.cpp
src/display/qt_adapter/ProjectionWidget.hpp
src/tls/Credentials.cpp
src/transport/tls_adapter/TlsRecordCodec.cpp
tests/display/dummy_display_test.cpp
tests/perf/fixtures/wired_perf/fake_receiver.py
tests/perf/fixtures/wired_perf/nominal_capture.json
tests/perf/fixtures/wired_perf/overload_capture.json
tests/perf/receiver_timing_test.cpp
tests/perf/test_continuity_regressions.py
tests/perf/test_gate_regressions.py
tests/perf/test_review_regressions.py
tests/perf/test_wired_perf.py
tests/tls/credential_probe.cpp
tests/tls/credentials.py
tools/perf/README.md
tools/perf/aa-projection-perf.schema.json
tools/perf/aa-projection-perf.sh
tools/perf/aa_perf_report.py
tools/perf/aa_perf_schema.py
tools/perf/aa_projection_collect.py
tools/wired-receiver/main.cpp
```

These remain PR56 timing/perf/paint and already-published certificate-chain
support, not recovery files reverted from main. Compared to starting local
PR56, the reconciliation imports the 20 PR55 dependency paths plus this receipt.

### Current focused host gates and limitations

On the source tree above, using the existing host-dev build directory:

- `cmake --preset host-dev`: exit 0 (existing upstream CMake deprecation warning).
- `cmake --build --preset host-dev`: exit 0; 37 incremental build steps,
  including receiver, recovery executable and receiver timing test.
- Serial `ctest --test-dir build/host-dev -j1 --output-on-failure -R REGEX`:

| Exact REGEX | Result | Log suffix |
| --- | --- | --- |
| `^(wired_perf_|dummy_display_)` | 29/29, 87.24 s | `perf-display.log` |
| `^(wired_receiver|wired_deadline|wired_e2e_(av_sink|microphone|control|revoke|recovery|auth|input_wire|happy|trust_link)|audio_pipeline_.*|wired_e2e_live_qa_gates)$` | 16/16, 40.17 s (`wired_e2e_recovery` has no registration; actual `wired_recovery` is covered below) | `receiver.log` |
| `^(wired_recovery_|usb_aoa_identity)` | 9/9, 167.91 s | `recovery.log` |
| `^(wired_recovery|wired_e2e_failures)$` | 2/2, 0.03 s | `recovery-smoke.log` |

Total: 56/56 focused CTests, not a full-suite result. Durable local logs are
`build/host-dev/task32-pr55-{configure,build,perf-display,receiver,recovery,recovery-smoke,syntax}.log`.
Test tool timeouts were at least 300000 ms. Harness tests use fake SSH locally;
no target SSH, ADB, hardware, trust-store or credential access was performed.

`bash -n` passed for the perf, live-QA and recovery-QA scripts. Python
`py_compile` passed for both QA gate suites, perf tools/tests and fake receiver,
with bytecode confined to `build/host-dev/task32-pycache`. C++ LSP error checks
passed for receiver main, app implementation directory (3 C++ files), wired E2E
directory (13 C++ files), USB adapter headers (5), USB endpoint implementation,
session/link/USB public headers and USB identity test. Python LSP was attempted
but basedpyright is unavailable (previous installation declined); CMake LSP is
not configured. Syntax checks and host configure/build provide those gates,
not an unsupported claim of all-language LSP coverage.

This is source-preserving reconciliation, with no new parser, helper, boundary,
type escape hatch, logging schema or budget change. Inherited large receiver
and recovery harness files are deliberately not refactored in this merge.
Existing timing and recovery behavior tests are reused rather than adding
tests that merely restate the conflict edits.

The full nonlong suite is explicitly NOT RUN and remains reserved for a
SEPARATE fresh worker later. No publication, PR mutation, auto/admin merge,
hardware soak, target build, acoustic-output or physical-scanout qualification
is claimed. Completion endpoints remain consumer-write handoff and Qt paint,
not speaker output or physical presentation.
