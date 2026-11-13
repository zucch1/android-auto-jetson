# Architecture and module layout

> **Current status: module architecture and boundary contracts are fully implemented.** The headers under `include/aa/` define the public interfaces and boundary contracts; implementations live in `src/**` and `tools/**`. The architecture boundaries are mechanically verified by `ctest -R architecture_boundary` (`tests/architecture/boundary_scan.py`).

## Processes

- `aa-wired-receiver` (unprivileged user session; no systemd unit for it is packaged yet): Android Auto protocol and session state, media handling, session D-Bus control/status service (`org.custom.AndroidAutoReceiver`), bounded video socket, and input socket.
- `aa-system-helper` (`aa-system-helper.service`, privileged, narrowly scoped): NetworkManager access point management, USB policy status, BlueZ profile registration and RFCOMM bootstrap mediation. The receiver reaches it only through `aa::helper::HelperClient`; no privileged tool is called directly.
- `aa-dummy-display` (consumer-side decode helper and test display): GStreamer hardware/software H.264 decode pipeline, Qt6 presentation, and a 5 ms precise timer pump (`tools/dummy-display/pump_timer.hpp`) for A/V synchronization.
- Bounded `AF_UNIX` `SOCK_SEQPACKET` channels: Video channel carrying encoded H.264 access units (timestamps, sequence numbers, IDR flags, drop-to-IDR recovery, 16 KiB fragment payload, 4 MiB frame limit, 56-byte header) and input channel for touch and key input reports.

## Module map

| Module | Public header | Owns | Implemented by |
|---|---|---|---|
| core | `include/aa/core/*` | typed errors, `Result`, strong ids/time, cancellation, logging boundary, session thread | implemented (`src/core/*`) |
| transport | `include/aa/transport/*` | framed-message transport, byte endpoints, USB AOA adapter, TCP and TLS adapters | implemented (`src/transport/*`, `src/transport/usb_adapter`, `src/transport/tls_adapter`) |
| protocol | `include/aa/protocol/*` | message/channel shape, channel roles, AASDK adapter, service and input validation | implemented (`src/protocol/*`, `src/protocol/aasdk_adapter`) |
| session | `include/aa/session/*` | session state machine, lifecycle deadlines, event dispatch | implemented (`src/session/*`) |
| channels | `include/aa/channels/*` | capability advertisement, service dispatch, AV/input/sensor/microphone sinks | implemented (`src/channels/*`) |
| ipc | `include/aa/ipc/*` | D-Bus control surface contract (`org.custom.AndroidAutoReceiver1`), pairing gate, video socket, input socket | implemented (`src/ipc/*`, `src/ipc/video`, `src/ipc/dbus_adapter`) |
| audio | `include/aa/audio/*` | multi-role PCM audio routing and sink boundary, A/V sync gating; `pulsesink` is the default and only supported real-time route (`pipewiresink` is refused on PipeWire 1.0.5 / Ubuntu noble) | implemented (`src/audio/*`, `src/audio/gstreamer_adapter`) |
| trust | `include/aa/trust/*` | persistent identity allowlist, atomic persistence, fail-closed admission (wired identifiers are spoofable) | implemented (`src/trust/*`) |
| config | `include/aa/config/*` | typed validated configuration boundary, fail-closed jurisdiction rules | implemented (`src/config/*`) |
| diagnostics | `include/aa/diagnostics/*` | structured JSON events, privacy redaction (salted pseudonyms), ring buffers | implemented (`src/diagnostics/*`) |
| helper | `include/aa/helper/*` | privileged helper client/service, NetworkManager AP worker, BlueZ AA RFCOMM bootstrap | implemented (`src/helper/*`, `src/helper/bluez_adapter`, `src/helper/nm`) |
| display | `include/aa/display/*` | Qt6 presentation adapter, 5 ms precise timer pump, telemetry writer | implemented (`src/display/*`, `tools/dummy-display/`) |
| consumer | `include/aa/consumer/*` | consumer-side GStreamer H.264 hardware/software decode pipeline | implemented (`src/consumer/*`) |
| input | `include/aa/input/*` | input event injection, capability advertisement, vehicle sensors service | implemented (`src/input/*`) |
| replay | `include/aa/replay/*` | deterministic session recording, verification, and fixture playback | implemented (`src/replay/*`) |
| app | `include/aa/app/*` | wired receiver session (`WiredSession`, `WiredLink`), wireless handoff eligibility/link | implemented (`src/app/*`, `tools/wired-receiver/`) |

## Error model

- `aa::ErrorCode` is a stable, project-owned enum (values grouped in per-module numeric bands; existing values never change meaning). External failure kinds (errno, OpenSSL, AASDK, protobuf) are converted into these codes at private adapter boundaries and never cross a public interface.
- `aa::Error` carries the code only; its message is a stable description. Dynamic diagnostic data goes into pre-redacted log fields.
- `aa::core::Result<T>` is a C++20 `std::variant<T, Error>` (no C++23 `std::expected` anywhere in this codebase). `Result<void>` is `std::variant<std::monostate, Error>`. Callers branch on `has_value()`.

## Threading and ownership

- **Session thread** (`aa::core::SessionThread`): the single serialized owner of all session state (state machine, channel registry, active consumer, trust decisions). Other threads only `post()` tasks; posted tasks run FIFO, never concurrently, and must not throw. `request_stop()` drops queued work and fires the C++20 stop token that transports and helper calls observe.
- **Transport I/O**: each `aa::transport::Transport` owns its fd/USB handle/SSL object exclusively; it is called from the session thread and returns whole frames or typed errors. Raw handles never leave the adapter.
- **Audio thread**: owns the `aa::audio::AudioSink` (GStreamer adapter using `pulsesink` [default and only supported real-time route via `pipewire-pulse`; `pipewiresink` is refused]); PCM chunk spans are valid only for the duration of `write()`.
- **Display pump**: Qt event loop runs on a 5 ms precise timer cadence (`tools/dummy-display/pump_timer.hpp`) to bound schedule-to-present latency and ensure A/V synchronization.
- **IPC adapters**: D-Bus and socket adapters own their connections on their own threads and post parsed requests to the session thread.
- **config**: immutable after `aa::config::validate()`; owned by the entry point.
- **diagnostics/log sinks**: owned by the entry point; `emit()`/`write()` are legal from any thread.

## Logging and privacy boundary

Modules log only through `aa::core::Logger` (no backend in any public header). Messages are stable literals; dynamic values are fields that have already crossed `aa::diagnostics::redact_identifier()` — phone identifiers, Bluetooth MACs, locations and credentials never appear raw in logs or events. Redaction produces a stable per-process pseudonym (`id-<hex>`, salted) so events correlate inside one run without carrying the identifier. Captures and secrets are never committed.

## Include boundary (enforced)

`tests/architecture/boundary_scan.py` scans the real module trees (`include/aa/**`, `src/**`) and is registered as the `architecture_boundary` CTest suite. Its rules:

1. **No public header** may include Qt/QML, GStreamer, BlueZ, NetworkManager, AASDK, protobuf or OpenSSL. This is the protocol boundary as well: nothing under `include/aa/protocol/` may include those families or `aa/tls/**`.
2. **`aa/tls` stays confined** (task-8 legacy): its public headers expose OpenSSL and may be used only by the TLS module itself (`src/tls/**`) or by private adapters. Protocol consumers must never include it — the TLS transport adapter does (`src/transport/tls_adapter/`).
3. **External implementations belong in private adapters**: in `src/**`, the external families above are legal only under adapter directories (path segment `adapter`, `adapters`, or `*_adapter`, e.g. `src/protocol/aasdk_adapter/`). Everything else in `src/**` is project-owned logic and stays free of external headers.

The scanner matches include directives with full `#`/`include` whitespace tolerance (`# include`, `#\tinclude\t<...>`, `#include<...>`) and tokenizes comments/string literals away first, so commented spellings do not false-match and literals cannot hide or fake a directive. Every negative CTest case writes a real temporary source with a forbidden include and requires the scanner to fail closed on it; known limitation: macro-indirected includes are not resolved.

## Transport order

1. Wired USB / Android Open Accessory.
2. Receiver-hosted wireless via a dedicated fixed-channel 5 GHz access point (explicit jurisdiction handling; fail-closed channel validation; jurisdiction must be install-time confirmed or the AP never starts).

## Wireless handoff

The host-tested receiver listener and helper-owned RFCOMM bootstrap are implemented. The helper alone handles AAW framing and credentials; the receiver uses authenticated bootstrap RPCs and a fenced NetworkManager AP snapshot. A negotiated AAP WifiCredentials service is metadata-only and distinct from that bootstrap. Each accepted TCP link reuses a fresh `WiredSession` with AAP encapsulated TLS, not an outer TLS transport. See [wireless-handoff.md](docs/wireless-handoff.md) for authorization, lifetimes, and revocation.

**Active Blocker:** A shared PSK cannot cryptographically bind the incoming TCP connection to the authenticated Bluetooth peer, so production rejects unbound connections with `transport_unauthorized_peer` (`src/app/WirelessLink.cpp`); wireless admission is not delivered in production. Host attestation is test-only; real-phone and Jetson qualification remain **TARGET BLOCKED**.

## Contract budgets referenced by interfaces

- Video: 1280x720@30 primary, 800x480@30 fallback (`aa::channels::CapabilityProfile::bench_defaults()`).
- Video socket: 16 KiB datagram payload, 4 MiB encoded access unit, 56-byte fragment header (`aa::ipc` — the wire format).
- Audio: ≤200 ms latency, ≤50 ms A/V skew (`aa::audio`).
- Display pump cadence: 5 ms precise timer (`tools/dummy-display/pump_timer.hpp`), bounding schedule-to-present delay to pass the 50 ms A/V gate (PR #81).
- A/V sync formula: `abs((video_done - video_stamp) - (audio_done - audio_begin))` schedule-to-completion latency difference.
- Audio routing default: `pulsesink` via `pipewire-pulse` (PR #88) is the default and only supported real-time route (`--audio-sink automatic` selects `pulsesink`; `--audio-sink pipewire` fails at startup with `audio_sink_unavailable`, see [audio-quantum.md](docs/audio-quantum.md)).
- `bench_defaults()` carries only the two video profiles. The wired receiver's service list (`service_descriptors` in `tools/wired-receiver/main.cpp`) advertises video, media, guidance and system audio, input, sensors and a 16 kHz mono microphone.

## Scope boundaries

- This repository owns the receiver sidecar and its test consumer only; it does not own any production infotainment UI, autostart, or display activation.
- Low-rate control/status over session D-Bus; bulk media over the `AF_UNIX` socket; no high-rate audio/video over D-Bus.
- Dependencies are pinned and vendored with provenance and license review (GPL-3.0-or-later compatible).
