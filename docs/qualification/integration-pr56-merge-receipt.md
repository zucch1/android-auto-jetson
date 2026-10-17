# Integration PR56 Merge Receipt

- **Old immutable pin (pre-merge)**: `5dec44a3e62d8b7b6fcd438d8438f73eef8408bb`
- **Merge commit**: `81042de` (forward-only merge of final PR56 source)
- **Incoming commit (final PR56)**: `3d31da3` (includes `73d2a7f` and `0c607ea`)

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

## Diagnostics

- LSP (clangd) on repository: 0 errors; 3 unused-include warnings in `.omo/integration-negative` (not part of the merged tree).
- Shell syntax (`bash -n`) on `tools/wired-qa/run-wired-live-qa.sh` and `tools/wired-qa/run-wired-recovery-qa.sh`: OK.
- Python syntax (`python3 -m py_compile`) on `tests/wired_e2e/test_live_qa_gates.py`, `tests/usb/test_probe_binary.py`, `tests/usb/test_usb_policy.py`, `tests/replay/test_fixture_qa.py`: OK.

## Build and test results

- Configure: `cmake --preset host-dev` succeeded.
- Build: `cmake --build build/host-dev` succeeded (56/56 targets).
- Focused tests (`ctest --preset host-dev -R "wired_perf|video_telemetry|video_outlet|test_privacy|test_wired|wired_recovery" --timeout 300000`): **25/25 passed** (224.44 sec). Full suite deferred to a separate worker.

## Remaining blockers

- Normal merge of final PR56 on GitHub is still blocked by running CI (unchanged; not waited on).
- Squash ancestry recording can follow once the squash merge actually lands.
