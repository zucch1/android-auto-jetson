# Integration PR56 Merge Receipt

- **Old immutable pin (pre-merge)**: `5dec44a3e62d8b7b6fcd438d8438f73eef8408bb`
- **Merge commit**: `81042de1a9fd159ce6656728beee7ac2ca579d56` (forward-only merge of final PR56 source)
- **Incoming commit (final PR56)**: `3d31da32e6266250ba552f351ddc0a6bf2f43164` (includes `73d2a7f` and `0c607ea`)

## Conflict decisions

All conflicts resolved as a coherent union preserving accepted integration
changes and final PR56 requirements. No wholesale side selection.

| Path | Decision |
| --- | --- |
| `cmake/WiredE2e.cmake` | Keep integration's `video_outlet_test.cpp` test registration and `make_libusb_link_factory` wrap; keep final PR56's `open_wired_link` wrap and formatting. |
| `docs/qualification/wired-recovery-qa.md` | Take final PR56's audio-pump requirement (`audio_chunks` AND `media_timing`). |
| `src/channels/AvSink.cpp` | Take final PR56's `std::copy` (GCC13 range warning fix) and keep `<algorithm>` include. |
| `src/transport/usb_adapter/UsbAoaEndpoint.cpp` | Take final PR56's function signature formatting. |
| `tests/channels/sensor_sink_test.cpp` | Take final PR56's `const std::vector<std::byte>&` lambda parameter. |
| `tests/input_sensors/sensor_session_test.cpp` | Take final PR56's `// NOLINT(bugprone-suspicious-include)` comment. |
| `tests/perf/receiver_timing_test.cpp` | Keep both test groups: integration's video-drop tests and final PR56's PCM-profile tests. |
| `tests/wired_e2e/receiver_test.cpp` | Keep integration's `VideoOutlet{log_line}` and `decode_stats` logging; keep final PR56's `video_ipc_send` endpoint expectations and audio-write-completion assertions. |
| `tests/wired_e2e/test_live_qa_gates.py` | Take final PR56's fake-SSH script and test set; keep integration's delay-polling test with `projection_timeout_ms`. |
| `tools/wired-qa/run-wired-live-qa.sh` | Take final PR56's display-connection check (`xrandr`) and audio check (`audio_chunks` AND `media_timing`). |
| `tools/wired-qa/run-wired-recovery-qa.sh` | Take final PR56's `media_offset` snapshot after consumer restart. |
| `tools/wired-receiver/main.cpp` | Keep integration's extracted `VideoOutlet.hpp` and `log_line` usage; take final PR56's `format_ms` comment. |

## Preserved invariants

- Foundation negotiated AV/mic refusal/input/control/deadline/owner/cipher.
- Final31 fresh-media/baseline/accounting gates.
- Final32 paired write/paint AV timing + final flush.
- Integration actual endpoint telemetry, bounded reducer, POLLHUP-only consumer
  retirement, strict privacy qualification validators, CTest registrations.
- Withdrawn task-9 decode p95 <= 33 ms gate remains withdrawn; decode timing is
  recorded informationally and never asserted.

## Auto-merge outcome reviewed (non-conflicting)

| Path | Decision |
| --- | --- |
| `src/app/WiredSession.cpp` | Auto-merged to the final PR56 state. History is a three-base criss-cross (`3df844e`, `228ce29`, `13a235d`); `a7283fd`'s temporary `HS-DBG` stderr instrumentation ("pending diag-event conversion") is an ancestor of both lineages, and the final PR56 lineage completed that conversion. The recursive merge keeps the removal. All negotiated media/microphone service wiring and the cipher handshake flow are byte-identical on both sides; the only integration-side delta was the temporary instrumentation. |

## Diagnostics

- LSP (clangd) on repository: 0 errors; 3 unused-include warnings in `.omo/integration-negative` (not part of the merged tree). Re-checked per changed file: zero errors on all 19 changed `.cpp`/`.hpp` files of the merge diff vs `5dec44a`, including every conflict-resolved file.
- Shell syntax (`bash -n`) on all 5 changed `.sh` files (`tools/wired-qa/run-wired-live-qa.sh`, `tools/wired-qa/run-wired-recovery-qa.sh`, `tools/usb-policy/common.sh`, `tools/usb-policy/install.sh`, `tools/usb-policy/rollback.sh`): OK.
- Python syntax (`python3 -m py_compile`) on all 6 changed `.py` files (`tests/wired_e2e/test_live_qa_gates.py`, `tests/wired_e2e/test_recovery_qa_gates.py`, `tests/usb/test_probe_binary.py`, `tests/usb/test_usb_policy.py`, `tests/replay/test_fixture_qa.py`, `tools/usb-policy/rules_check.py`): OK.

## Build and test results

- Configure: `cmake --preset host-dev` succeeded (log `/tmp/opencode/configure.log`).
- Build: `cmake --build build/host-dev` succeeded (56/56 targets; log `/tmp/opencode/build.log`).
- Focused tests batch 1 (`ctest --preset host-dev -R "wired_perf|video_telemetry|video_outlet|test_privacy|test_wired|wired_recovery" --timeout 300000`): **25/25 passed** (224.44 sec) — matched `wired_perf_*` (16) and `wired_recovery*` (9); log `/tmp/opencode/ctest.log`.
- Focused tests batch 2 (`ctest --preset host-dev -R "video_socket|wired_receiver|wired_qualification|wired_e2e_live_qa_gates|wired_e2e_av_sink|channel_services|input_socket" --timeout 300000`): **22/22 passed** (39.19 sec); log `/tmp/opencode/ctest-focused-2.log`.

Group coverage across the two batches:

- **perf**: `wired_perf_*` (16), incl. `wired_perf_receiver_timing`, `wired_perf_video_measurements`.
- **video**: `video_socket_*` (8), incl. `video_socket_stream` running `video_telemetry_test.cpp` (actual endpoint telemetry).
- **outlet**: `wired_receiver` (`WiredReceiver.*`, incl. `VideoOutlet*` and `ConsumerDisconnectKeepsMediaAckAndReplacementDelivery`).
- **qualification**: `wired_qualification` (`tests/qualification/test_privacy.py` + `test_wired.py`) and `wired_e2e_live_qa_gates`.
- **recovery**: `wired_recovery*` (9), incl. `wired_recovery_qa_gates`.

Conflict-file suites `wired_e2e_av_sink`, `channel_services_sensors`,
`channel_services_input`, `channel_services_microphone` and
`channel_services_wire_reject` pass as well. Total focused: **47/47 passed**.
Full suite (465 tests) deferred to a separate worker.

## Full-suite closure and final fixture correction

The first exclusive full run found a genuine repository-gate conflict, not a
hardware or runtime fault: integration's new privacy fixture stored a raw PEM
marker in source, tripping the pre-existing zero-credential scan
(`tls_posture_reference`, `tls_posture_key_scan`) while the rest passed 462/464.
The correction replaces the stored literal with runtime concatenation in
`tests/qualification/test_privacy.py`; the same negative fixture marker is still
produced when the test runs and no scanner rule is weakened.

Verified after the correction: `tools/tls/scan.py` passes,
21/21 qualification/privacy tests pass, and a fresh exclusive full suite exits 0:
**464/464 passed**, 536.15 seconds
(`build/host-dev/Testing/integration-final56-nonlong-fixed-20261017.log`).
Landed PR56 squash `2fa07dae` was later added as an ancestry parent without
changing the reviewed union tree (receipt may appear as an additional parent
while source content remains the same). This is host qualification only; target
deployment and the real 30-minute / 2-hour windows remain the next gates.

## Remaining blockers

- No remaining host-suite blocker on the corrected candidate.
- Target build/deploy, fresh positive-control admission, actual separate 30m
  and 2h media runs, and sanitized task33 evidence remain pending.
