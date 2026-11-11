# Architecture

**Status: core module architecture and typed error model are implemented
(task 14).** The headers under `include/aa/` are the real contracts; behavior
they do not yet have lands in tasks 15-27 as named below. Everything in this
file matches code in this tree, and `ctest -R architecture_boundary` proves
the include boundary mechanically.

## Processes

- `aa-receiver.service` (unprivileged user session): Android Auto protocol and
  session state, media handling, D-Bus control/status surface.
- `aa-system-helper.service` (privileged, narrowly scoped): NetworkManager
  access point management, USB policy, ModemManager exclusion, BlueZ profile
  registration and pairing mediation. The receiver reaches it only through
  `aa::helper::HelperClient`; no privileged tool is called directly.
- Consumer-side GStreamer decode helper and dummy test display.
- Bounded `AF_UNIX` `SOCK_SEQPACKET` channel carrying encoded H.264 access
  units (timestamps, sequence numbers, IDR flags, drop-to-IDR recovery).

## Module map

| Module | Public header | Owns | Filled by |
|---|---|---|---|
| core | `include/aa/core/*` | typed errors, `Result`, strong ids/time, cancellation, logging boundary, session thread | implemented (task 14) |
| transport | `aa/transport/Transport.hpp` | one framed-message channel per phone | task 16 (USB/TCP/TLS adapters) |
| protocol | `aa/protocol/Protocol.hpp` | message/channel shape, channel roles | task 15 (AASDK adapter) |
| session | `aa/session/Session.hpp` | session state machine | task 17 (timeouts, policy) |
| channels | `aa/channels/Services.hpp` | capability advertisement, service dispatch | task 18 (real handlers) |
| ipc | `aa/ipc/Control.hpp`, `aa/ipc/VideoSocket.hpp` | D-Bus control surface contract, video socket wire contract | tasks 21, 22 |
| audio | `aa/audio/Audio.hpp` | PCM sink boundary, A/V budgets | task 25 (PipeWire adapter) |
| trust | `aa/trust/Trust.hpp` | approve-once phone store, fail-closed admission | task 27 |
| config | `aa/config/Config.hpp` | typed validated configuration | task 41+ (file/env loading) |
| diagnostics | `aa/diagnostics/Diagnostics.hpp` | structured events, privacy redaction | task 19 (full schema/redaction) |
| helper | `aa/helper/HelperClient.hpp` | privileged-operation client contract | task 34 (RPC adapter) |

## Error model

- `aa::ErrorCode` is a stable, project-owned enum (values grouped in per-module
  numeric bands; existing values never change meaning). External failure kinds
  (errno, OpenSSL, AASDK, protobuf) are converted into these codes at private
  adapter boundaries and never cross a public interface.
- `aa::Error` carries the code only; its message is a stable description.
  Dynamic diagnostic data goes into pre-redacted log fields.
- `aa::core::Result<T>` is a C++20 `std::variant<T, Error>` (no C++23
  `std::expected` anywhere in this codebase). `Result<void>` is
  `std::variant<std::monostate, Error>`. Callers branch on `has_value()`.

## Threading and ownership

- **Session thread** (`aa::core::SessionThread`): the single serialized owner
  of all session state (state machine, channel registry, active consumer,
  trust decisions). Other threads only `post()` tasks; posted tasks run FIFO,
  never concurrently, and must not throw. `request_stop()` drops queued work
  and fires the C++20 stop token that transports and helper calls observe.
- **Transport I/O**: each `aa::transport::Transport` owns its fd/USB handle/SSL
  object exclusively; it is called from the session thread and returns whole
  frames or typed errors. Raw handles never leave the adapter.
- **Audio thread**: owns the `aa::audio::AudioSink` (PipeWire adapter); PCM
  chunk spans are valid only for the duration of `write()`.
- **IPC adapters**: D-Bus and socket adapters own their connections on their
  own threads and post parsed requests to the session thread.
- **config**: immutable after `aa::config::validate()`; owned by the entry point.
- **diagnostics/log sinks**: owned by the entry point; `emit()`/`write()` are
  legal from any thread.

## Logging and privacy boundary

Modules log only through `aa::core::Logger` (no backend in any public header).
Messages are stable literals; dynamic values are fields that have already
crossed `aa::diagnostics::redact_identifier()` — phone identifiers, Bluetooth
MACs, locations and credentials never appear raw in logs or events. Redaction
produces a stable per-process pseudonym (`id-<hex>`, salted) so events correlate
inside one run without carrying the identifier. Captures and secrets are never
committed.

## Include boundary (enforced)

`tests/architecture/boundary_scan.py` scans the real module trees
(`include/aa/**`, `src/**`) and is registered as the `architecture_boundary`
CTest suite. Its rules:

1. **No public header** may include Qt/QML, GStreamer, BlueZ, NetworkManager,
   AASDK, protobuf or OpenSSL. This is the protocol boundary as well: nothing
   under `include/aa/protocol/` may include those families or `aa/tls/**`.
2. **`aa/tls` stays confined** (task-8 legacy): its public headers expose
   OpenSSL and may be used only by the TLS module itself (`src/tls/**`) or by
   private adapters. Protocol consumers must never include it — the TLS
   transport adapter does (task 16).
3. **External implementations belong in private adapters**: in `src/**`, the
   external families above are legal only under adapter directories (path
   segment `adapter`, `adapters`, or `*_adapter`, e.g.
   `src/protocol/aasdk_adapter/`). Everything else in `src/**` is
   project-owned logic and stays free of external headers.

The scanner matches include directives with full `#`/`include` whitespace
tolerance (`# include`, `#\tinclude\t<...>`, `#include<...>`) and tokenizes
comments/string literals away first, so commented spellings do not false-match
and literals cannot hide or fake a directive. Every negative CTest case writes
a real temporary source with a forbidden include and requires the scanner to
fail closed on it; known limitation: macro-indirected includes are not
resolved.

## Transport order

1. Wired USB / Android Open Accessory.
2. Receiver-hosted wireless via a dedicated fixed-channel 5 GHz access point
   (explicit jurisdiction handling; fail-closed channel validation;
   jurisdiction must be install-time confirmed or the AP never starts).

## Wireless handoff

Task37 implements the host-tested receiver listener and helper-owned RFCOMM
bootstrap. The helper alone handles AAW framing and credentials; the receiver
uses authenticated bootstrap RPCs and a fenced NetworkManager AP snapshot.
A negotiated AAP WifiCredentials service is metadata-only and distinct from
that bootstrap. Each accepted TCP link reuses a fresh `WiredSession` with AAP
encapsulated TLS, not an outer TLS transport. See
[wireless-handoff.md](docs/wireless-handoff.md) for authorization, lifetimes,
and revocation. **BLOCKED:** a shared PSK cannot bind TCP to the Bluetooth
peer, so production rejects unbound connections and wireless admission is not
delivered. Host attestation is test-only; real-phone and Jetson qualification
remain **TARGET BLOCKED**.

## Contract budgets referenced by interfaces

- Video: 1280x720@30 primary, 800x480@30 fallback
  (`aa::channels::CapabilityProfile::bench_defaults()`).
- Video socket: 16 KiB datagram payload, 4 MiB encoded access unit, 56-byte
  fragment header (`aa::ipc` — the task-10 spike wire format).
- Audio: ≤200 ms latency, ≤50 ms A/V skew (`aa::audio`).
- Bench defaults advertise video, the four audio roles, bench input and
  diagnostics only — microphone, vehicle sensors and the projection
  passthrough services are not advertised until qualified.

## Scope boundaries

- This repository owns the receiver sidecar and its test consumer only; it
  does not own any production infotainment UI, autostart, or display
  activation.
- Low-rate control/status over session D-Bus; bulk media over the AF_UNIX
  socket; no high-rate audio/video over D-Bus.
- Dependencies are pinned and vendored with provenance and license review
  (GPL-3.0-or-later compatible).

## Receiver screen description (single panel, issue #86)

`aa-wired-receiver` accepts `--screen-<field> N` alongside its existing
`--port`, `--store`, `--video-socket` and `--audio-sink` options. Values are
unsigned decimal integers (no sign, whitespace or trailing characters).
Invalid syntax, unknown screen fields or unsafe geometry exit with code 2,
name the field on stderr, and fail before opening USB, sockets or the store.

| Field | Accepted values | Default announcement |
| --- | --- | --- |
| `density` | Android bucket: 120, 160, 240, 320, 480, 640 | 160 |
| `real-density` | 40–1000 DPI, explicitly measured by installer | absent |
| `pixel-aspect-ratio-e4` | 5000–20000 (10000 = square pixels) | absent |
| `decoder-additional-depth` | 0–8 extra buffered frames | absent |
| `width-margin` | 0–799 total horizontal stream margin in pixels | absent |
| `height-margin` | 0–479 total vertical stream margin in pixels | absent |
| `theme` | 0 automatic, 1 light, 2 dark | absent |
| `margins-{top,bottom,left,right}` | pixels, each edge and opposing sum smaller than active extent | absent |
| `content-insets-{top,bottom,left,right}` | same bounds | absent |
| `stable-content-insets-{top,bottom,left,right}` | same bounds | absent |

The bounds are conservative receiver policy, not claimed protocol limits.
One description is applied to both existing 1280×720 and 800×480 offers.
Margin bounds and each inset rectangle must fit **both** offers; active extent
is stream extent minus the total stream margin. Setting any edge creates its
inset rectangle with the remaining edges zero. Other optional fields stay
absent unless set, preserving the legacy discovery bytes exactly by default.
The `screen_description` startup event logs all configured fields (null means
not advertised). Density is an explicit bucket override; it is not automatically inferred from
DPI. Do not substitute physical DPI for an Android density bucket.

Example announcement experiment:

```sh
aa-wired-receiver --port 1-2.1 --screen-density 240 \
  --screen-real-density 210 --screen-height-margin 80 \
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
display consumer crop them. Treat nonzero margins as experimental until those
paths are completed. No multi-display support, obsolete viewing distance,
new wire extensions, codec negotiation or hardware decode is added here.
Phone screenshots at 120/160/240, non-16:9 coverage, accurate edge touch, and
1800/7200-second qualification remain required before issue #86 can close.

Replay video services may carry an optional `screen` object whose numeric keys
are the same field names used after `--screen-`. Export preserves optional-field
presence; default descriptions omit the object, keeping existing fixture bytes
and hashes unchanged. The loader uses the same screen parser and geometry
validation as discovery and rejects unknown keys and unsafe values.
