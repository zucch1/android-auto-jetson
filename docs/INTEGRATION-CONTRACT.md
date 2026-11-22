<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Android Auto Infotainment Integration Contract

- **Contract version:** `1.0.0`
- **Receiver D-Bus contract:** `org.custom.AndroidAutoReceiver1` (`1.1.0`)
- **System helper D-Bus contract:** `org.custom.AndroidAutoSystemHelper1` (`1.5.0`)
- **Video socket ABI:** `AF_UNIX` `SOCK_SEQPACKET`, protocol version `1` (data header v1 / v2 profile-bearing)
- **Input socket ABI:** `AF_UNIX` `SOCK_SEQPACKET`, protocol version `1`

This document is the versioned integration boundary between `zucch1/android-auto-jetson` (Android Auto receiver, AA) and `zucch1/infotainment` (Mazda6 head unit, on the Jetson at `~/Desktop/infotainment`). Every interface, constant, unit, and file path below is derived from the in-tree source and verified by `tools/contract/check_integration.py`.

---

## 1. Ownership and Scope Boundaries

Android Auto is a separate subproject of the head unit. It arrives when ready, and nothing in infotainment waits for it or depends on it at boot.

### Infotainment owns

- **Screen and app host:** Display ownership and window/surface composition (Weston `ivi-shell` or Qt Wayland Compositor, still undecided; see `zucch1/infotainment#9`).
- **Audio sources, volume, focus, and output routing:** Master volume, PipeWire/WirePlumber routing to vehicle outputs, and system-wide audio focus arbitration.
- **Bluetooth HFP link:** The Hands-Free Profile (`0000111e`, WirePlumber on RFCOMM channel 7). AA never pages, connects, disconnects, cancels pairing, or removes Bluetooth devices.
- **All user-facing prompts:** Wired and wireless enrolment yes/no dialogs and Bluetooth numeric-comparison confirmation. Infotainment's UI is the single registered D-Bus consumer; AA ships only minimal reference consumers (`tools/dummy-display/main.cpp`, `tools/wired-enrol/main.cpp`) for tests and owner bring-up.

### Android Auto owns

- **Session D-Bus control contract:** `org.custom.AndroidAutoReceiver1` (`docs/dbus/org.custom.AndroidAutoReceiver1.xml`).
- **AF_UNIX video and input framing ABI:** `include/aa/ipc/VideoSocket.hpp`, `include/aa/ipc/VideoChannel.hpp`, `include/aa/ipc/InputSocket.hpp`, `include/aa/ipc/InputChannel.hpp`.
- **Consumer decode helper:** `libaareceiver-consumer` (`include/aa/consumer/Decode.hpp`).
- **Privileged system helper and replacement API:** `aa-system-helper` (`docs/dbus/org.custom.AndroidAutoSystemHelper1.xml`, `docs/system-helper.md`).
- **Autostart of its own units:** System and user units shipped by AA packaging.
- **Privacy and redaction rules:** Enforced across logs, D-Bus events, trust stores, and evidence exports (`tools/qualification/check_privacy.py`).

No claim of Google certification, official SDK status, road approval, microphone/voice/HFP-call qualification, or physical touch/HMI qualification is made.

---

## 2. Session D-Bus Control Contract

Control and status only. Audio, video, and every other high-rate stream stay off D-Bus.

| Field | Value |
| --- | --- |
| Bus | Session D-Bus (`DBUS_SESSION_BUS_ADDRESS`) |
| Service name | `org.custom.AndroidAutoReceiver` |
| Object path | `/org/custom/AndroidAutoReceiver` |
| Interface | `org.custom.AndroidAutoReceiver1` |
| Contract SemVer | `1.1.0` |
| Authority XML | `docs/dbus/org.custom.AndroidAutoReceiver1.xml` |
| Schema validator | `tools/ipc/dbus_contract.py` (`tools/ipc/dbus_contract_schema.py`) |
| Max message bytes | `65536` (`64 KiB`, `kMaxMessageBytes` in `src/ipc/dbus_adapter/BusWire.hpp`) |

### Authorization and Single-Consumer Rule

- Exactly one registered session consumer may hold projection control at a time (`include/aa/ipc/Control.hpp`, `include/aa/ipc/PeerCredentials.hpp`).
- The caller must run as the active session Unix UID (`GetConnectionUnixUser`) and currently own `consumer_name` (`GetNameOwner`). Unknown callers, wrong UIDs, and unowned names fail closed.
- The session bus socket is authenticated via `SO_PEERCRED` (caller UID or root) before any D-Bus byte is sent; when `DBUS_SESSION_BUS_ADDRESS` carries `guid=`, the server GUID must match (`docs/system-helper.md`).
- Owner-loss reconciliation (`ControlService::refresh_consumer_owner`) runs at least once per second; loss of `consumer_name` stops projection, closes pending pairing requests, and tears down consumer authority.
- Messages larger than `65536` bytes are drained and dropped without a reply, emitting `DiagnosticEvent` (`dbus_message_dropped`, rate-limited to once per 10 s after the first drop).

### Versioning Rules

- The **major** version is encoded in the interface name (`org.custom.AndroidAutoReceiver1`, `kContractMajor = 1` in `include/aa/ipc/Contract.hpp`). Any unsupported major (e.g., `org.custom.AndroidAutoReceiver2`) fails closed with `org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor`.
- The **minor** version grows for backwards-compatible additions (methods, typed errors, capability keys). `1.1.0` added `GetPairingCandidate`, `Error.NotWiredCandidate`, and `Error.NotDurable`. Consumers feature-detect by reading the `ContractSemVer` property (or `contract.semver` in `GetCapabilities`) before calling `1.1.0+` methods.
- The **patch** version fixes behavior or wording without changing wire shape.

### Receiver Methods

| Method | In | Out | Notes |
| --- | --- | --- | --- |
| `RegisterConsumer` | `s` | `t` | Registers caller owning `consumer_name` as the exclusive session consumer; returns `consumer_id`. |
| `UnregisterConsumer` | `` | `` | Drops caller's registration and stops projection first. |
| `StartProjection` | `` | `` | Opens projection control for the registered consumer. |
| `StopProjection` | `` | `` | Closes projection control. |
| `RequestAddPhone` | `` | `t` | Issues a one-time `request_id` (`PairingGate::kMaxPending = 8`, `kIssuedBudget = 64` per gate lifetime) and emits `PairingRequest`. |
| `ConfirmPhonePairing` | `t` | `` | Confirms pending `request_id` (same consumer, within window). Wired non-durable sync fails with `Error.NotDurable`. |
| `CancelPhonePairing` | `t` | `` | Cancels pending request or closes open pairing window without persisting approval. |
| `ForgetPhone` | `t` | `` | Forgets paired `phone_id`, rotates shared AP credentials, and demotes wireless bond to `revoked-bonds` without removing the BlueZ pairing. |
| `GetState` | `` | `st` | Returns `projection_state` (`"stopped"` or `"projecting"`) and active `consumer_id` (`0` if none). |
| `GetCapabilities` | `` | `a{ss}` | Static capability map: `contract.interface`, `contract.semver`, `consumer.exclusive`, `pairing.one_time_request_ids`, `display.viewport`, `diagnostics.events`. |
| `SetDisplayViewport` | `uuu` | `` | Consumer-reported `width` (`1..8192`), `height` (`1..8192`), `dpi` (`1..2000`). |
| `Ping` | `` | `t` | Liveness probe returning receiver monotonic clock in nanoseconds. |
| `GetPairingComparison` | `t` | `a{ss}` | Consumer-only retrieval during wireless enrolment: `challenge_id`, `comparison` (6 digits), `address`, `adapter`. Never broadcast. |
| `RespondPairingComparison` | `ttb` | `` | Explicit human accept/reject for `request_id` and `challenge_id`. One-use. |
| `GetPairingCandidate` | `t` | `a{ss}` | Consumer-only wired candidate description (`1.1.0`, `docs/wired-enrolment.md`): `transport` (`"wired"`), `label` (per-process pseudonym), `strength`. Fails `Error.NotWiredCandidate` on wireless requests. |
| `RequestIdrRecovery` | `` | `` | Requests a fresh H.264/video IDR keyframe from the active phone session while projecting. |

### Receiver Signals

| Signal | Signature | Notes |
| --- | --- | --- |
| `PairingRequest` | `t` | One-time `request_id` awaiting `ConfirmPhonePairing`. |
| `PairingOpened` | `t` | Pairing window opened (or wired approval committed) for `request_id`. |
| `PairingClosed` | `t` | Pending request or open window closed for `request_id`. |
| `StateChanged` | `st` | Mirrors `GetState`: `projection_state`, `consumer_id`. |
| `DiagnosticEvent` | `sst` | Low-rate structured diagnostic: `severity` (`"info"`, `"warning"`, `"error"`), `event`, numeric `subject_id`. Never carries identifiers or secrets. |

### Receiver Properties

| Property | Type | Access | Value |
| --- | --- | --- | --- |
| `ContractSemVer` | `s` | `read` | `1.1.0` |

### Receiver Errors

Defined in `src/ipc/dbus_adapter/DispatchReply.hpp`:

| Error | Meaning |
| --- | --- |
| `org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor` | Caller addressed an unsupported `org.custom.AndroidAutoReceiverN` major version. |
| `org.custom.AndroidAutoReceiver1.Error.PeerUnauthorized` | Caller UID mismatch, unowned consumer name, or not the active consumer. |
| `org.custom.AndroidAutoReceiver1.Error.ConsumerRejected` | Another consumer is already registered, no consumer is registered, or pairing request/window rejected. |
| `org.custom.AndroidAutoReceiver1.Error.MalformedRequest` | Invalid argument types, bounds (e.g. viewport out of range), or unknown request id. |
| `org.custom.AndroidAutoReceiver1.Error.QueueFull` | Pending pairing table full (`8`) or gate-lifetime request budget spent (`64`; restart required). |
| `org.custom.AndroidAutoReceiver1.Error.IllegalTransition` | Invalid state transition (e.g., `StartProjection` while projecting, `RequestIdrRecovery` while stopped). |
| `org.custom.AndroidAutoReceiver1.Error.NotWiredCandidate` | `GetPairingCandidate` called on a request with no wired candidate (e.g., wireless request). Request stays pending. |
| `org.custom.AndroidAutoReceiver1.Error.NotDurable` | Wired `ConfirmPhonePairing` wrote the record, but directory `fsync` failed after bounded retry. Request is spent; do not report approved. |

---

## 3. AF_UNIX Video and Input Framing ABI and Geometry Negotiation

Both media/input sockets use `AF_UNIX` `SOCK_SEQPACKET`, live in `$XDG_RUNTIME_DIR/android-auto-receiver/` (fallback `/run/user/$UID/android-auto-receiver/`, directory mode `0700`, socket node `0600`), and enforce same-UID `SO_PEERCRED` admission on both server and client (`transport_unauthorized_peer` on mismatch).

### Video Socket ABI

- **Path:** `android-auto-receiver/video.sock` (`include/aa/ipc/VideoChannel.hpp`, `src/ipc/video/VideoChannel.cpp`)
- **Direction:** Receiver binds/listens (`VideoServer` / `VideoProducer`); infotainment consumer connects (`connect_video_consumer` / `VideoConsumer`).
- **Codec authority:** `include/aa/ipc/VideoSocket.hpp`, `src/ipc/video/VideoCodec.cpp`.

#### Video Framing Constants

| Constant | Value | Meaning |
| --- | --- | --- |
| `kVideoProtocolVersion` | `1` | Base wire protocol version (data records also accept version `2` when carrying `VideoProfile` metadata). |
| `kVideoFrameMagic` | `0x41415631` | `"AAV1"` big-endian magic at offset 0 of every video fragment record. |
| `kVideoLimitsMagic` | `0x41414C31` | `"AAL1"` big-endian magic at offset 0 of the limits-exchange handshake record. |
| `kFrameHeaderBytes` | `56` | Fixed big-endian header length preceding fragment payload bytes. |
| `kLimitsRecordBytes` | `32` | Fixed big-endian limits-exchange record size. |
| `kLimitsRecordKind` | `1` | Limits-exchange record kind field (offset 8). |
| `kMinDatagramPayload` | `1024` | Minimum negotiable fragment payload size in bytes. |
| `kMaxDatagramPayload` | `16384` | Maximum negotiable fragment payload size in bytes (16 KiB). |
| `kMaxAccessUnitBytes` | `4194304` | Maximum reassembled access unit size in bytes (4 MiB). |
| `kSocketBudgetBytes` | `262144` | Default negotiated kernel socket buffer budget in bytes (256 KiB). |
| `kMinSocketBufferBytes` | `262144` | Lower clamp for profile-scaled `SO_SNDBUF`/`SO_RCVBUF` sizing (256 KiB). |
| `kMaxSocketBufferBytes` | `4194304` | Upper clamp for profile-scaled `SO_SNDBUF`/`SO_RCVBUF` sizing (4 MiB). |

#### Video Handshake and Wire Layout (Big-Endian)

1. **Limits exchange (32 bytes, both sides immediately after `SO_PEERCRED` passes):**
   - `0..3`: magic (`0x41414C31`, `"AAL1"`)
   - `4..5`: protocol version (`u16 = 1`)
   - `6..7`: record size (`u16 = 32`)
   - `8..11`: kind (`u32 = 1`)
   - `12..15`: `max_datagram_payload` (`u32`, `1024..16384`)
   - `16..19`: `max_access_unit_bytes` (`u32`, `1..4194304`)
   - `20..23`: `socket_budget_bytes` (`u32`, `1..262144`)
   - `24..31`: two `u32` reserved fields (must be `0`)
   - Effective limits are the per-field `min(local, peer)`. Handshake failure closes the connection.
2. **Video fragment header (56 bytes, followed by `payload_bytes` of encoded H.264/H.265/VP9/AV1 data):**
   - `0..3`: magic (`0x41415631`, `"AAV1"`)
   - `4..5`: version (`u16`: `1` without profile metadata, `2` with profile metadata)
   - `6..7`: header size (`u16 = 56`)
   - `8..11`: `idr` (`u32`: `0` non-IDR, `1` IDR)
   - `12..19`: `frame` (`u64` access-unit id)
   - `20..23`: `index` (`u32` fragment index, `0..count-1`)
   - `24..27`: `count` (`u32` total fragments for this access unit)
   - `28..35`: `sequence` (`u64` monotonic per-fragment wire sequence)
   - `36..43`: `timestamp_ns` (`u64` common monotonic A/V clock)
   - `44..47`: `frame_bytes` (`u32` full access-unit byte length)
   - `48..51`: `payload_bytes` (`u32` fragment payload byte length)
   - `52..55`: `metadata` (`u32`: `0` for version `1`; `(resolution_index << 16) | (fps << 8) | codec` for version `2`, where `resolution_index` is 1-based into `aa::protocol::kVideoResolutions` `1..9`, `fps` is `30` or `60`, and `codec` is `3`=h264, `5`=vp9, `6`=av1, `7`=h265 per `include/aa/protocol/Protocol.hpp`).
3. **Loss, backpressure, and EOF:**
   - Slow consumers lose data at access-unit granularity with drop-to-IDR resync; malformed/oversize/missing fragments also drive drop-to-IDR.
   - End of stream is `shutdown(SHUT_WR)` from the producer, never a zero-length record.

### Input Socket ABI

- **Path:** `android-auto-receiver/input.sock` (`include/aa/ipc/InputSocket.hpp`, `include/aa/ipc/InputChannel.hpp`, `src/ipc/input/InputChannel.cpp`)
- **Direction:** Receiver binds/listens (`InputServer` / `InputEndpoint`); infotainment consumer connects nonblocking and sends (`InputSender`).

#### Input Framing Constants

| Constant | Value | Meaning |
| --- | --- | --- |
| `kInputRecordBytes` | `24` | Fixed big-endian input datagram size in bytes. |
| `kInputProtocolVersion` | `1` | Input wire protocol version. |
| `kInputRecordMagic` | `0x41414931` | `"AAI1"` big-endian magic at offset 0. |
| `kInputKindKey` | `1` | Keyboard / D-pad / steering-wheel Android keycode event. |
| `kInputKindTouch` | `2` | Absolute-viewport touch event. |

#### Input Wire Layout (24 Bytes, Big-Endian)

- `0..3`: magic (`0x41414931`, `"AAI1"`)
- `4..5`: version (`u16 = 1`)
- `6..7`: record size (`u16 = 24`)
- `8..11`: `kind` (`u32`: `1` key, `2` touch)
- `12..15`: `a` (`u32`: Android `keycode` for key; `x` in absolute viewport pixels for touch)
- `16..19`: `b` (`u32`: `pressed` `0|1` for key; `y` in absolute viewport pixels for touch)
- `20..23`: `c` (`u32`: `0` for key; `TouchAction` `0`=down, `1`=move, `2`=up for touch)

`InputSender` is strictly nonblocking so a congested receiver (`transport_queue_full`) never stalls the consumer's GUI thread; congestion is counted without tearing down the socket.

### Geometry Negotiation

- **Consumer viewport:** The registered consumer reports its display geometry via `SetDisplayViewport(width, height, dpi)` (`width`, `height` in `1..8192`; `dpi` in `1..2000`, enforced in `src/ipc/ControlService.cpp`).
- **Stream profiles:** The receiver offers an ordered list of `VideoProfile` entries (`include/aa/config/VideoProfiles.hpp`, `docs/video-pipeline.md`), defaulting to `1280x720@30:h264, 800x480@30:h264` with `1280x720@30:h264` as the universal fallback unless exact-profiles-only is requested. Supported wire resolutions (`include/aa/protocol/Protocol.hpp`): `800x480`, `1280x720`, `1920x1080`, `2560x1440`, `3840x2160` and their portrait transposes.
- **Screen insets and scaling:** `aa::config::Screen` (`include/aa/config/Screen.hpp`, default `density = 160`) defines pixel margins and content insets in primary-profile pixels and scales them per axis (`scale_screen`) to every offered profile.
- **Touch coordinates:** `TouchEvent` coordinates sent over `input.sock` are absolute pixels in the active viewport/stream geometry.

---

## 4. Consumer-Helper Usage (`libaareceiver-consumer`)

Infotainment's C++ AA adapter should link `libaareceiver-consumer` (`include/aa/consumer/Decode.hpp`) to decode `aa::ipc::VideoAccessUnit` payloads received from `VideoConsumer`:

1. **Probe (optional, safe before open):** Call `DecodeHelper::probe_backends(codec)` to inspect runtime GStreamer element viability (`nvv4l2decoder` + `nvvidconv`, software decoders). Presence is measured at runtime, never assumed from JetPack.
2. **Open:** Call `DecodeHelper::open(DecodeConfig{caps, backend, on_frame, on_completion})`.
   - `DecodeBackend::automatic` selects NVIDIA (`nvv4l2decoder` + `nvvidconv`) when the complete output path is viable, otherwise falls back to software at `open()` time only.
   - `DecodeBackend::nvidia` never silently downgrades; it fails typed (`channel_decode_unavailable`).
3. **Stream loop (single-threaded, caller-driven):**
   - Submit reassembled access units with `submit(au)`. `timestamp_ns` must be strictly increasing and session-unique (no reset on IDR recovery).
   - Drain decoded I420 frames with `pump(wait)`. `on_frame(const DecodedFrame&)` executes on the `pump()` caller's thread, never on a GStreamer streaming thread. Each `DecodedFrame` carries tightly packed I420 (`Y + U + V`) and the original `frame`, `sequence`, `timestamp_ns`, `width`, `height`, and `fps`.
4. **Error and resync handling:**
   - `ipc_queue_full` from `submit()`: retryable byte-backlog rejection; timestamp is not consumed.
   - `ipc_consumer_rejected` from `submit()` or `pump()`: non-retryable resync notice (5-second operational retention bound or capacity saturation). Discard non-IDRs, call `RequestIdrRecovery` over D-Bus, and resume at a newer IDR.
   - `channel_decode_failed` or `channel_caps_unsupported` after `open()`: terminal for that `DecodeHelper` instance. Close, reopen, and request a fresh IDR.
5. **Teardown:** Call `end_of_stream()`, drain via `pump()`, and `close()` (idempotent; destructor also closes).

Reference consumers shipped in-tree (`aa-dummy-display` and `aa-wired-enrol`) are test and bring-up tools only; infotainment's UI is the production consumer.

---

## 5. Autostart Ownership

AA owns autostart of its own units. Infotainment starts independently and must never block boot or UI startup on any AA unit.

### Packaging and Policy Units

| Artifact | Scope | Purpose |
| --- | --- | --- |
| `packaging/systemd/aa-system-helper.service` | System (`root`, `multi-user.target`) | Runs `/usr/libexec/aa-system-helper` (`Requires=dbus.service`, `After=dbus.service`, `StateDirectory=aa-system-helper`, `LimitCORE=0`, `UnsetEnvironment=G_DBUS_DEBUG`). |
| `packaging/dbus/org.custom.AndroidAutoSystemHelper.conf` | System D-Bus | Reserves `org.custom.AndroidAutoSystemHelper` for `root` and denies method delivery by default until a per-UID fragment for `session.uid` is provisioned. |
| `packaging/ap.conf.example` | System (`/etc/android-auto-receiver/ap.conf`) | AP interface, regulatory country, jurisdiction confirmation, and projection port (`5277`) template. |
| `tools/usb-policy/70-android-auto-receiver.rules` | System udev | Scoped phone + AOA (`18d1:2d00`/`2d01`) ACL (`plugdev`, `0660`, `uaccess`) and `ID_MM_DEVICE_IGNORE=1` on any USB port (`#133`). |

### User vs System Boundaries

- **System scope (root):** `aa-system-helper.service`, the system D-Bus policy + per-UID delivery fragment, `/etc/android-auto-receiver/session.uid`, `/etc/android-auto-receiver/receiver.sha256`, `/etc/android-auto-receiver/ap.conf`, and `70-android-auto-receiver.rules`.
- **User-session scope (session UID):** `aa-wired-receiver` and `aa-wireless-receiver` run as the active session user on the session D-Bus (packaging of user receiver units is tracked in `#189`; any receiver unit must set `LimitCORE=0`, per `docs/tls-credentials.md`).
- **Infotainment scope:** Starts `infotainmentd`, `infotainment-ui`, and its display runtime (Weston or Qt Compositor) with no `Requires=`/`After=` dependency on AA.

---

## 6. Privileged System Helper Replacement API

If infotainment or a future platform service replaces `aa-system-helper`, it must implement the exact `org.custom.AndroidAutoSystemHelper1` wire contract (`docs/dbus/org.custom.AndroidAutoSystemHelper1.xml`, `include/aa/helper/BusService.hpp`, `include/aa/helper/SystemHelper.hpp`, `docs/system-helper.md`).

| Field | Value |
| --- | --- |
| Bus | System D-Bus (`unix:path=/run/dbus/system_bus_socket`, `SO_PEERCRED` UID `0`) |
| Service name | `org.custom.AndroidAutoSystemHelper` |
| Object path | `/org/custom/AndroidAutoSystemHelper` |
| Interface | `org.custom.AndroidAutoSystemHelper1` |
| Contract SemVer | `1.5.0` |
| Authority XML | `docs/dbus/org.custom.AndroidAutoSystemHelper1.xml` |

### Helper Methods

| Method | In | Out | Purpose |
| --- | --- | --- | --- |
| `PollBootstrapV1` | `tt` | `utssu` | Polls active RFCOMM bootstrap handoff handle, peer address/adapter, and `local_failure` (`0..14`). |
| `GetBootstrapWaitV1` | `tt` | `ut` | `1.5.0` (`#160`): read-only wait diagnostic (`0` none, `1` `no_bluetooth_link`). Grants nothing. |
| `AuthorizeBootstrapV1` | `tttuu` | `ut` | Authorizes or denies RFCOMM bootstrap (`allow`, `listener_ready`). |
| `GetAPCredentialsV1` | `ttt` | `utssssut` | Returns pinned AP activation snapshot (`ssid`, `password`, `bssid`, `receiver_ip`, `port`, `activation_generation`). |
| `RevokeAP1` | `t` | `ut` | Rotates shared AP credentials and evicts AP stations for a forgotten `phone_id`. |
| `PollPeerV1` | `` | `utss` | Returns exactly one helper-observed public BR/EDR candidate (`handle`, `bond_address`, `adapter_identity`). |
| `BindPeerV1` | `ttt` | `ut` | Binds an observed peer handle to `(consumer_generation, request_source)` before confirmation. |
| `ReconnectPeerV1` | `t` | `ut` | Opens a 60 s reconnect window for an already-admitted, bonded peer handle. |
| `RevokePeerV1` | `ss` | `ut` | Demotes an exact `(bond_address, adapter_identity)` to `revoked-bonds` without removing the BlueZ pairing (`#136`). |
| `Issue` | `ttuu` | `ut` | Issues a single-use, 5 s random grant bound to `(receiver, generation, source, action, channel)`. |
| `Execute` | `uut` | `ut` | Executes strict typed action `1..10` (`acquire_ap`, `open_pairing`, `release_ap`, `ap_status`, `usb_status`, `modem_status`, `register_profile`, `unregister_profile`, reserved `9`, `close_pairing`). |
| `PairingBondConfirmedV1` | `tt` | `ut` | Read-only verification that numeric comparison was accepted and the exact bond exists. |
| `PairingBondAcknowledgedV1` | `tt` | `ut` | Marks the verified bond provisional after receiver-side checks pass (writes nothing durable). |
| `PairingBondCommittedV1` | `tt` | `ut` | Admits the provisional bond into `managed-bonds` after the receiver's durable trust write. |
| `ReconcileBondsV1` | `a(ss)` | `ut` | Reconciles helper bond state to the receiver's authoritative approved list (at most `256` entries). |
| `GetPairingComparisonV1` | `tt` | `utuss` | Retrieves one-time numeric comparison challenge (`challenge_id`, 6-digit `comparison`, `bond_address`, `adapter_identity`). |
| `RespondPairingComparisonV1` | `tttu` | `ut` | Submits explicit human accept/reject for the active comparison challenge. |
| `Invalidate` | `tt` | `ut` | Invalidates grants and leases for `(generation, source)` (`0, 0` clears all). |

### Replacement Requirements

- **Errors and states:** Wire error codes `0` (success), `1000` (unavailable), `1001` (rejected), `1002` (protocol mismatch). States: `0` (inactive), `1` (active), `2` (unavailable). Unknown interfaces, paths, members, or signatures fail closed.
- **Receiver authentication:** Every call authenticates the daemon-stamped unique sender via `GetConnectionUnixUser`, `GetConnectionUnixProcessID`, `/proc/PID/status` UIDs, live starttime, and full SHA-256 of `/proc/PID/exe` against `/etc/android-auto-receiver/session.uid` and `/etc/android-auto-receiver/receiver.sha256`.
- **Grant and bond invariants:** Max `16` pending grants, `4096` lifetime replay history, `5 s` grant TTL, non-DFS UNII-1 AP channels (`36`, `40`, `44`, `48`), durable `managed-bonds` / `pending-removals` / `revoked-bonds` under `/var/lib/aa-system-helper` (`0700`).
- **BlueZ restraint:** Never call `Connect`, `ConnectProfile`, `Disconnect`, `DisconnectProfile`, `CancelPairing`, or `RemoveDevice` (`#136`, `#160`).

---

## 7. Audio Streams and Focus Requests

- **Streams AA emits:** AA decodes four AAP audio roles (`include/aa/audio/Audio.hpp`: `media`, `guidance`, `system`, `speech`; PCM or AAC-LC ADTS), ducks and mixes them internally (`include/aa/audio/AudioMix.hpp`: `media` `1.0` / `0.25` (-12 dB) ducked by `guidance`/`speech` within a `200 ms` window; `guidance` `1.0`; `system` `0.5` (-6 dB); `speech` `1.0`), and emits one mixed `48000 Hz`, 2-channel, 16-bit PCM stream (`include/aa/audio/AudioRouter.hpp`).
- **Audio backend:** AA routes through GStreamer `pulsesink` (`pipewire-pulse`) to the current default PipeWire output. Native `pipewiresink` is refused on PipeWire 1.0.5 / Ubuntu noble (`#80`).
- **Focus requests:** Inside the AAP protocol (`include/aa/protocol/Messages.hpp`), the phone sends `AudioFocusRequest` (`ControlMessageId::audio_focus_request = 18`: `gain`, `gain_transient`, `gain_transient_may_duck`, `release`) and AA replies with `AudioFocusNotification` (`ControlMessageId::audio_focus_notification = 19`). Currently `src/app/WiredSession.cpp` answers focus requests locally (`release -> loss`, otherwise `gain`), and `AudioRouter::set_muted()` provides global mute.
- **Infotainment arbitration:** Infotainment owns system-wide audio focus, volume, and output routing. Under receiver contract `1.1.0`, infotainment arbitrates AA audio at the PipeWire/WirePlumber layer (mute, duck, cork, or route AA's `pulsesink` stream) and calls `StopProjection` when AA must yield completely. Fine-grained external focus forwarding over D-Bus is deferred to a future compatible minor version (`1.2.0+`).

---

## 8. HFP Precondition

- Infotainment owns the Bluetooth HFP Hands-Free link (`0000111e`, WirePlumber on RFCOMM channel 7).
- A bonded phone initiates the wireless AA RFCOMM bootstrap (`4de17a00-52cb-11e6-bdf4-0800200c9a66`) only over an existing Bluetooth link (`docs/wireless-handoff.md`, `#160`).
- AA never pages, connects, or disconnects HFP. During a reconnect window, AA waits for the phone to dial.
- If the attempt deadline expires with no live bootstrap and `Device1.Connected` false for the admitted phone, the receiver queries `GetBootstrapWaitV1` and emits `wireless_admission_expired` with `reason: no_bluetooth_link`, `phase: bluetooth_link`, `detail: "waiting for HFP (owned by infotainment)"`.
- Forgetting a phone in AA (`ForgetPhone`) revokes AA trust and moves the bond to `revoked-bonds`, but leaves the BlueZ pairing intact (`#136`). To re-enrol a forgotten phone in AA, the user unpairs it in infotainment's Bluetooth settings first, then enrols it again.

---

## 9. Privacy Rules

The following data must **never** be logged, exported in diagnostics/evidence, broadcast over D-Bus signals (`DiagnosticEvent`), or committed to git (`tools/qualification/check_privacy.py`, `docs/tls-credentials.md`, `docs/system-helper.md`):

- **Credentials and keys:** Head-unit TLS private keys and certificates (`headunit.key`, `headunit.crt`, PEM blocks), auth tokens, and bearer credentials.
- **Wi-Fi AP secrets:** AP SSID, WPA2 PSK, BSSID, and `/var/lib/aa-system-helper/ap.credentials` contents. The helper holds PSKs in `explicit_bzero`-wiped fixed buffers (`nm::Psk`), enforces `LimitCORE=0`, and strips `G_DBUS_DEBUG`.
- **Bluetooth secrets and addresses:** Link keys, raw Bluetooth MAC addresses, and raw adapter addresses in logs or broadcast events.
- **Trust stores:** Contents of `approved-phones.json`, `managed-bonds`, `pending-removals`, and `revoked-bonds`.
- **Phone and location identifiers:** USB serial numbers, AOA strings, USB ports, IMEI, IMSI, Android ID, MEID, UDID, and raw GNSS/location coordinates.
- **Raw captures:** Packet or media dumps (`.pcap`, `.pcapng`, `.h264`, `.aac`, `.pcm`, `usbmon`, `snoop`).

Permitted telemetry: opaque per-process pseudonyms (`label` from `GetPairingCandidate`), numeric `PhoneId`/`request_id`/`subject_id`, and consumer-only (never broadcast) responses to `GetPairingComparison` and `GetPairingCandidate` during active explicit enrolment.

---

## 10. Infotainment Handoff Checklist

Exact amendments required in `zucch1/infotainment` when integrating Android Auto (none are implemented by this repository):

### Settings

- [ ] Add an Android Auto settings page behind `AppState` (QML declarative only): show AA availability/degraded state when `org.custom.AndroidAutoReceiver` is absent, list/forget approved phones (`ForgetPhone`), and drive explicit enrolment (`RequestAddPhone` -> `GetPairingCandidate` for wired or `GetPairingComparison` for wireless -> human yes/no modal -> `ConfirmPhonePairing` / `CancelPhonePairing` / `RespondPairingComparison`).
- [ ] Keep Bluetooth/HFP pairing and unpairing under infotainment Settings so the owner can connect HFP (`#160`) and unpair a forgotten phone before re-enrolment (`#136`).
- [ ] Feature-detect receiver contract `1.1.0` via `ContractSemVer` / `GetCapabilities` before calling `GetPairingCandidate`, and handle `Error.NotDurable` by never reporting an unconfirmed wired approval as succeeded.

### App-Host

- [ ] Implement a C++ AA consumer adapter behind `AppState` that registers as the exclusive session consumer (`RegisterConsumer`), sends `SetDisplayViewport(width, height, dpi)`, connects to `video.sock` (`aa::ipc::connect_video_consumer`), decodes frames via `aa::consumer::DecodeHelper`, renders I420 frames into the app-host surface, and forwards normalized touch/key events over `input.sock` (`aa::ipc::InputSender`).
- [ ] Support the selected display runtime (`zucch1/infotainment#9`: Weston `ivi-shell` client surface or Qt Wayland Compositor / embedded QML texture under `eglfs`) without placing D-Bus or socket I/O in QML.
- [ ] Wire `StartProjection` / `StopProjection` to app foreground/background transitions and call `RequestIdrRecovery` on surface resume or `ipc_consumer_rejected` resync.
- [ ] Arbitrate AA's `pulsesink` PipeWire stream for volume, mute, ducking against infotainment navigation/media, and output routing.

### Startup

- [ ] Keep `infotainmentd`, `infotainment-ui`, and display-runtime units completely independent of `aa-system-helper.service` and AA receiver units (no `Requires=`, `Wants=`, or `After=` on AA).
- [ ] Watch `org.custom.AndroidAutoReceiver` name ownership on the session bus: attach lazily when it appears, re-register after receiver restarts, degrade cleanly on vanish, and rate-limit retries to at most once per second.
- [ ] Bring up HFP (`0000111e` via WirePlumber) for bonded phones so wireless AA reconnect windows can complete without AA paging Bluetooth.

### Guard Amendments

- [ ] `tests/scope/check_network_listeners.py` (in `zucch1/infotainment`): AA's consumer adapter only calls `connect()` on `AF_UNIX` (`video.sock`, `input.sock`) and requires no TCP/UDP listener. Amend this guard **only if** infotainment adds an `AF_UNIX`-only Qt Wayland Compositor (`bind`/`listen` on a Wayland socket) — explicitly allowlist `AF_UNIX`-only sockets without `AF_INET`/`AF_INET6`, never weakening the `127.0.0.1` loopback-only rule for IP listeners.
- [ ] `tests/smoke/check_daemon_no_network.sh` (in `zucch1/infotainment`): Keep `infotainmentd` network-free; AA's AP/TCP listener stays in `aa-wireless-receiver` / `aa-system-helper`, never in `infotainmentd`.
- [ ] `tests/smoke/check_ui_daemon_contract_seam.cmake` and `tests/navigation/check_qt_purity.py` (in `zucch1/infotainment`): Keep all AA D-Bus, socket, and GStreamer decode calls inside the C++ adapter behind `AppState` so QML purity and the `infotainment-ui` <-> `infotainmentd` contract seam remain intact.
- [ ] `tests/scaffold/check_repository_layout.cmake` (in `zucch1/infotainment`): Register any new AA consumer adapter directory in the repository layout guard.
- [ ] Provisioning hygiene: Provision `/etc/android-auto-receiver/session.uid`, `/etc/android-auto-receiver/receiver.sha256`, `/etc/android-auto-receiver/ap.conf`, and the per-UID D-Bus delivery fragment via AA packaging/deployment; never commit them to either repository.
