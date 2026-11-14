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
- The display controller retains one complete access unit rejected by the
  decoder with `ipc_queue_full`, pumps the decoder, and retries that unit before
  reading more video. This makes the controller lossless against retryable
  decoder backpressure, but does not guarantee end-to-end losslessness:
  sustained saturation can fill the bounded kernel socket buffers upstream.
  The video producer then reports `dropped_backpressure`, drops subsequent
  non-IDR units with `dropped_waiting_idr`, and resumes on the next IDR that
  can be sent; the consumer resynchronizes on that IDR.
- Audio: ≤200 ms latency, ≤50 ms A/V skew (`aa::audio`).
- Display pump cadence: 5 ms precise timer (`tools/dummy-display/pump_timer.hpp`), bounding schedule-to-present delay to pass the 50 ms A/V gate (PR #81).
- A/V sync formula: `abs((video_done - video_stamp) - (audio_done - audio_begin))` schedule-to-completion latency difference.
- Audio routing default: `pulsesink` via `pipewire-pulse` (PR #88) is the default and only supported real-time route (`--audio-sink automatic` selects `pulsesink`; `--audio-sink pipewire` fails at startup with `audio_sink_unavailable`, see [audio-quantum.md](docs/audio-quantum.md)).
- `bench_defaults()` carries only the two video profiles. The wired receiver's service list (`service_descriptors` in `tools/wired-receiver/main.cpp`) advertises video, media, guidance and system audio, input, sensors and a 16 kHz mono microphone.

## Scope boundaries

- This repository owns the receiver sidecar and its test consumer only; it does not own any production infotainment UI, autostart, or display activation.
- Low-rate control/status over session D-Bus; bulk media over the `AF_UNIX` socket; no high-rate audio/video over D-Bus.
- Dependencies are pinned and vendored with provenance and license review (GPL-3.0-or-later compatible).

## Receiver screen description (single panel, issue #86)

`aa-wired-receiver` accepts `--screen-<field> N` alongside its existing
`--port`, `--store`, `--video-socket` and `--audio-sink` options. Values are
unsigned decimal integers (no sign, whitespace or trailing characters).
Invalid syntax, unknown screen fields or unsafe geometry exit with code 2,
name the field on stderr, and fail before opening USB, sockets or the store.

| Field | Accepted values | Default announcement | Gate |
| --- | --- | --- | --- |
| `density` | Android bucket: 120, 160, 240, 320, 480, 640 | 160 | – |
| `real-density` | 40–1000 DPI, explicitly measured by installer | absent | – |
| `pixel-aspect-ratio-e4` | 5000–20000 (10000 = square pixels) | absent | experimental unless 10000 |
| `decoder-additional-depth` | 0–8 extra buffered frames | absent | – |
| `width-margin` | pixels of the primary profile, below its width | absent | experimental if nonzero |
| `height-margin` | pixels of the primary profile, below its height | absent | experimental if nonzero |
| `theme` | 0 automatic, 1 light, 2 dark | absent | – |
| `margins-{top,bottom,left,right}` | primary-profile pixels, each edge and opposing sum smaller than the active extent | absent | experimental if any edge nonzero |
| `content-insets-{top,bottom,left,right}` | same bounds | absent | – |
| `stable-content-insets-{top,bottom,left,right}` | same bounds | absent | – |

The bounds are conservative receiver policy, not claimed protocol limits.

**Per-profile meaning.** The receiver offers two stream profiles, 1280×720
(primary, listed first) and 800×480. Pixel quantities (`width-margin`,
`height-margin` and every inset edge) are **given in the primary profile's
pixels** and are scaled **per axis** to each other offered profile and rounded
half up: x (width margin, left, right) by 800/1280, y (height margin, top,
bottom) by 480/720. So `--screen-height-margin 120` announces 120 in the
1280×720 entry and 80 in the 800×480 entry: the same fraction of the panel
whichever profile the phone picks. This follows the protocol, where margins and
insets are "pixels from edge" of the stream the entry describes (open-android-auto
`UiConfigMessages.proto` `Insets`; `VideoConfigData.proto` margin fields), so
they cannot be one number shared by two stream sizes. Panel properties
(`density`, `real-density`, `pixel-aspect-ratio-e4`, `decoder-additional-depth`,
`theme`) are sent identically in every entry. Active extent is the stream extent
minus the total stream margin, and the screen is validated against **every**
offered profile after scaling (rounding can push a value that fits the primary
profile over the bound in the other, for example `margins-left 1275` with
`margins-right 4`); an invalid field exits 2 and names the field. A decoded
discovery response must carry the primary entry's screen scaled into each other
entry, otherwise it is malformed.

**Experimental gate.** Cropping the stream to its margins and mapping touch
through them do not exist yet: `TouchViewport{1280, 720}`
(`src/input/Capability.cpp`) is unchanged and `video_rect()` letterboxes the full
stream. A nonzero `width-margin`, `height-margin`, `margins-*` edge, or a
`pixel-aspect-ratio-e4` other than 10000 (the display does not stretch for it)
would therefore give a wrong touch mapping and a cropped or offset picture, so
they are refused with exit 2 (`screen field needs --screen-experimental-geometry:
<field>`) unless `--screen-experimental-geometry` is also given; then the
receiver logs a `screen_experimental` warning naming the field and the
consequence. `content-insets-*` and `stable-content-insets-*` describe areas the
phone keeps its own interactive content out of; the display still draws the whole
stream and touch maps over the whole stream, so a wrong value only wastes screen
and is not gated. `density`, `real-density`, `decoder-additional-depth` and
`theme` change only the phone's layout/buffering and are not gated either.

Setting any edge creates its inset rectangle; the other edges of that rectangle
are sent as **explicit zeros** (the proto2 default) and a decoder treats absent
edges as zero too: per-edge presence is canonicalized, not preserved, and what
the phone does with an absent versus zero edge is unverified. Other optional
fields stay absent unless set, preserving the legacy discovery bytes exactly by
default (golden test against the video entry `main` announced). A decoder
ignores the obsolete `viewing_distance` field, as before. The `screen_description`
startup event logs all configured fields in primary-profile pixels (null means
not advertised). Density is an explicit bucket override; it is not automatically
inferred from DPI. Do not substitute physical DPI for an Android density bucket.

Example announcement experiment:

```sh
aa-wired-receiver --port 1-2.1 --screen-density 240 \
  --screen-real-density 210 \
  --screen-content-insets-left 12 --screen-theme 2 \
  --screen-pixel-aspect-ratio-e4 10000 --screen-decoder-additional-depth 1
```

These options describe capabilities during service discovery; video setup
continues to use the existing fixed stream profiles. The pinned schema has no
separate panel refresh-rate or presentation-pacing field. Decoder depth is a
phone buffering hint, **not** vsync or a frame-rate negotiation control (#79,
#85). `theme` sets `ui_config.ui_theme`; automatic scheduling, sensor-driven
night mode and ambient-light sources are not implemented by this option.

Remaining issue #86 work: attached-panel/EDID detection, physical dimensions
and automatic density selection, viewport/sub-area presentation, cropping
stream margins and matching touchscreen mapping/type, and real-phone
verification. In particular, advertising margins does not make the current
display consumer crop them; nonzero margins stay behind
`--screen-experimental-geometry` until those paths are completed. No multi-display support, obsolete viewing distance,
new wire extensions, codec negotiation or hardware decode is added here.
Phone screenshots at 120/160/240, non-16:9 coverage, accurate edge touch, and
1800/7200-second qualification remain required before issue #86 can close.

Replay video services may carry an optional `screen` object whose numeric keys
are the same field names used after `--screen-` (pixel values in primary-profile
pixels). Export preserves optional-field presence; default descriptions omit the
object, keeping existing fixture bytes and hashes unchanged. The loader uses the
same screen parser and geometry validation as discovery and rejects unknown keys
and unsafe values. Only the canonical form is accepted, so load then export is
byte-stable: a `screen` object that loads as the default (`{}`, `{"density":160}`)
or lacks `density` is rejected. Fixtures are data, not command lines, so the
experimental gate does not apply to them.
