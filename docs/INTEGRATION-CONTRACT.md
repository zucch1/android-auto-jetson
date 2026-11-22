<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Android Auto Infotainment Integration Contract

- **Contract version:** `1.0.0`
- **Receiver D-Bus contract:** `org.custom.AndroidAutoReceiver1` (`1.1.0`)
- **System helper D-Bus contract:** `org.custom.AndroidAutoSystemHelper1` (`1.5.0`)
- **Video socket ABI:** `AF_UNIX` `SOCK_SEQPACKET`, protocol version `1` (data header v1 / v2 profile-bearing)
- **Input socket ABI:** `AF_UNIX` `SOCK_SEQPACKET`, protocol version `1`

This document is the versioned integration boundary between `zucch1/android-auto-jetson` (Android Auto receiver, AA) and `zucch1/infotainment` (Mazda6 head unit, on the Jetson at `~/Desktop/infotainment`). `tools/contract/check_integration.py` enforces agreement between this document and the in-tree source for the contract SemVer and section structure, receiver and system-helper D-Bus XML and C++ identity constants (`Contract.hpp`, `BusService.hpp`), custom receiver error names and max D-Bus message size, AF_UNIX video/input framing constants, the documented field offsets in the video-frame, input and limits encoders plus the video-frame decoder's timestamp read, and socket paths; for the helper's `aa-system-helper.service` it enforces the single-valued `[Service]`/`[Unit]`/`[Install]` directives it names (service user/group, `ExecStart`, `StateDirectory`, `LimitCORE`, `NoNewPrivileges`, `Requires=`/`After=dbus.service`, `WantedBy=multi-user.target`) and the **effective** `UnsetEnvironment=` list (a later empty assignment resets it); for `org.custom.AndroidAutoSystemHelper.conf` it enforces the **effective** policy — evaluated in document order, last matching rule wins — as default-deny for reference plus a root-only `own` allow, and fails closed on any policy shape it does not model; and it requires every in-tree backticked file path to exist. It does not verify every decoder field offset or the ordering of decoder reads (the encode/decode round trip is covered by the video codec and input socket unit tests under tests/, not by this checker), wildcard D-Bus rules (rejected as unsupported), the remaining unit hardening directives, D-Bus policy rules outside that modelled shape or bus-name scope, or any behavioural prose, runtime sequence, or external `zucch1/infotainment` checklist item — those are reviewed manually.

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
- **Calls available before registration:** `GetState`, `GetCapabilities`, `Ping`, `org.freedesktop.DBus.Properties` reads (`Get` / `GetAll` for `ContractSemVer`), and `org.freedesktop.DBus.Introspectable.Introspect` do not require a registered consumer (`src/ipc/dbus_adapter/ControlDispatcher.cpp`). `RegisterConsumer` is the one call that *creates* the registration: a caller that runs as the session UID and currently owns the `consumer_name` it supplies may register even while no consumer is active (`ControlService::register_consumer`; it rejects a second registration with `Error.ConsumerRejected`). Every other method requires the caller to be the active registered consumer.
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
| `StopProjection` | `` | `` | Closes projection control. Stops wireless projection; does **not** silence an active wired session (see Section 7). |
| `RequestAddPhone` | `` | `t` | Issues a one-time `request_id` (`PairingGate::kMaxPending = 8`, `kIssuedBudget = 64` per gate lifetime) and emits `PairingRequest`. Wireless: also prepares and binds a peer, which the helper refuses unless exactly one public BR/EDR candidate is already visible (see the enrolment sequence). |
| `ConfirmPhonePairing` | `t` | `` | Wireless: opens the 60 s comparison window and emits `PairingOpened`. Wired: commits candidate after `GetPairingCandidate` (`Error.NotDurable` if directory `fsync` fails). |
| `CancelPhonePairing` | `t` | `` | Cancels pending request or closes open pairing window without persisting approval. |
| `ForgetPhone` | `t` | `` | Forgets paired `phone_id`. Wireless: also rotates shared AP credentials and demotes the bond to `revoked-bonds` without removing the BlueZ pairing; on the wired control service (no helper backend) it is a local-store revocation only. See partial-outcome semantics below. |
| `GetState` | `` | `st` | Available before registration. Returns `projection_state` (`"stopped"` or `"projecting"`) and active `consumer_id` (`0` if none). |
| `GetCapabilities` | `` | `a{ss}` | Available before registration. Static capability map: `contract.interface`, `contract.semver`, `consumer.exclusive`, `pairing.one_time_request_ids`, `display.viewport`, `diagnostics.events`. |
| `SetDisplayViewport` | `uuu` | `` | Validates and stores consumer-reported `width` (`1..8192`), `height` (`1..8192`), `dpi` (`1..2000`). Does not negotiate stream or input geometry (see Section 3). |
| `Ping` | `` | `t` | Available before registration. Liveness probe returning receiver monotonic clock in nanoseconds. |
| `GetPairingComparison` | `t` | `a{ss}` | Consumer-only retrieval during an **open** wireless enrolment window (after `ConfirmPhonePairing` / `PairingOpened`): `challenge_id`, `comparison` (6 digits), `address`, `adapter`. Never broadcast. |
| `RespondPairingComparison` | `ttb` | `` | Explicit human accept/reject for `request_id` and `challenge_id`. One-use. Acknowledges the answer; bond verification and durable approval can complete afterwards. |
| `GetPairingCandidate` | `t` | `a{ss}` | Consumer-only wired candidate description (`1.1.0`, `docs/wired-enrolment.md`): `transport` (`"wired"`), `label` (per-process pseudonym, no persistent `phone_id`), `strength`. Fails `Error.NotWiredCandidate` on wireless requests. |
| `RequestIdrRecovery` | `` | `` | Requests a fresh H.264/video IDR keyframe from the active phone session while projecting. |

### Enrolment Sequences and `ForgetPhone` Semantics

Both enrolment flows keep all user-facing prompts in infotainment. Neither flow auto-approves a phone.

#### Wireless enrolment sequence (`src/ipc/ControlPairing.cpp`, `src/ipc/ControlComparison.cpp`)

1. **Precondition — exactly one visible public BR/EDR candidate:** `RequestAddPhone()` synchronously *prepares and binds* a peer (`BusClient::prepare` -> helper `PollPeerV1`/`BindPeerV1`). The helper's `observe()` refuses unless exactly one eligible device is in BlueZ's current inventory (paired devices count too) and fewer than 16 observations are outstanding (`devices.size() != 1` -> `helper_rejected`, surfaced as `org.freedesktop.DBus.Error.Failed`; `src/helper/bluez_adapter/Peers.cpp`, `src/helper/BusClient.cpp`). "Eligible" means a `public`-address BlueZ `Device1` (not a random/resolvable private address) on the single visible adapter. The UI must establish this condition *before* requesting: exactly one candidate discoverable and visible, which in practice means the phone is discoverable and no other eligible public-address device, paired or not, is in BlueZ's inventory. A refused request is "candidate set not ready", never a transient error to retry blindly (blind retries spend the one-time id and the helper's observation slots).
2. **Request:** Registered consumer calls `RequestAddPhone()` -> receives one-time `request_id` and `PairingRequest(request_id)`.
3. **Open enrolment window:** Registered consumer calls explicit `ConfirmPhonePairing(request_id)`, which activates the helper's 60 s pairing window and emits `PairingOpened(request_id)`. `GetPairingComparison` fails closed until this window is open and matches `request_id`.
4. **Wait for comparison:** Poll `GetPairingComparison(request_id)` until the phone's Bluetooth numeric comparison arrives (`challenge_id`, 6-digit `comparison`, `address`, `adapter`).
5. **Present to human:** Display the comparison in infotainment's UI and obtain an explicit accept or reject decision (keep raw `address`/`adapter`/`comparison` out of logs and telemetry).
6. **Respond with both IDs:** Call `RespondPairingComparison(request_id, challenge_id, accept)`. A successful return acknowledges the human answer and can return **before** bond verification (`PairingBondConfirmedV1`, `PairingBondAcknowledgedV1`) and durable approval (`PairingBondCommittedV1`) finish; subsequent reconciliation/commit runs asynchronously in `advance_enrolment()`.
7. **Commit outcomes differ by durability (`advance_enrolment()`, `src/ipc/ControlComparison.cpp`):**
   - **Durable trust write, helper commit fails:** the approval is **kept**, although the receiver still emits `pairing_commit_failed` for the helper step. It ends the pairing window and schedules bond reconciliation, which brings the helper in line with the stored approval. The phone is approved; do not send the owner through enrolment again.
   - **Trust write committed but not durable:** the receiver emits `pairing_commit_failed` and performs a best-effort retraction (store forget -> tracked revocation -> bond reconciliation). If the retracting write also fails, the approval can remain in the store and reconciliation keeps the helper consistent with it.
   - Receiver interface `1.1.0` gives the consumer no call that reports whether an approval persisted: `GetState` reports projection/consumer state and `GetPairingComparison` a live challenge. Until a status API exists (blocked on the same API decision as the approved-phone list), the UI must show a non-final "pairing finished; confirming" state and treat the next successful wireless admission of that phone as the confirmation, never render a stopped spinner as "approved" or as "failed".

#### Wired enrolment sequence (`docs/wired-enrolment.md`)

1. **Refused candidate:** An unapproved phone connects while `aa-wired-receiver --wired-enrolment` is running (with session bus enabled, not `--no-bus`). Admission refuses it (`transport_unauthorized_peer`) and opens a 120 s candidate window (`wired_enrolment_open`, `WiredEnrolment::kCandidateFreshness`) bound to that refused identity. The window never outlives the receiver run: it is `min(now + 120 s, the receiver's --duration-ms deadline)`, so a short `--duration-ms` truncates the candidate window. A refusal older than the window is stale (`RequestAddPhone` binds nothing).
2. **Request:** Registered consumer calls `RequestAddPhone()` -> receives one-time `request_id` and `PairingRequest(request_id)`. The request itself is fresh for 60 s (`PairingPolicy::kConfirmWindow`): the confirmation must land inside it or the request is spent.
3. **Inspect candidate:** Consumer calls `GetPairingCandidate(request_id)` (`1.1.0+`) -> receives `transport` (`"wired"`), `label` (per-process pseudonym matching `phone_id` in the refusal log, not a persistent `phone_id`), and `strength` (`"serial-backed"`). Confirming before calling `GetPairingCandidate` is refused and closes the request.
4. **Present to human:** Show `label` in infotainment's UI and obtain an explicit yes/no.
5. **Confirm or decline:** Call `ConfirmPhonePairing(request_id)` to approve durably (emits `PairingOpened`), or `CancelPhonePairing(request_id)` to decline without writing. If directory `fsync` fails after bounded retries, `ConfirmPhonePairing` returns `Error.NotDurable` and closes the request — never report approval as succeeded. A durable confirm *is* the completed enrolment: `WiredEnrolment::pairing_confirmed` sets its completion result while handling the call, and the receiver's enrolment loop then closes the bus and exits (`wired_enrolment_approved`, exit 0). Do **not** call `CancelPhonePairing` after a durable confirm — the receiver can exit before serving it and the approval needs no closing (it stays).
6. **Replug and restart:** The refused USB/AOA link is not reused after approval; replug the phone (or reset its USB functions) and restart `aa-wired-receiver` to admit the approved phone on any USB port.

#### Timing limits

- **120 s wired candidate window** (`WiredEnrolment::kCandidateFreshness`): a refusal older than that cannot bind a request.
- **60 s request freshness** (`PairingPolicy::kConfirmWindow`): a one-time `request_id` must be confirmed within 60 s of the request; the same 60 s bounds an open wireless comparison window, and a bound wireless candidate handle is itself only usable for 60 s (`BluezBackend::observe` observation expiry).
- **Duration truncation:** the wired candidate window ends at `min(now + 120 s, receiver --duration-ms deadline)`; a receiver started with a short `--duration-ms` offers a correspondingly shorter window.

#### `ForgetPhone` partial outcomes and approved-phone listing

- **No list API:** `org.custom.AndroidAutoReceiver1` (`1.1.0`) provides `ForgetPhone(t)` but no method to list approved phones or their `PhoneId` values, and `GetPairingCandidate` does not expose a persistent `phone_id`. Consumers must **never** read `approved-phones.json` directly (that violates the ownership and privacy boundary); approved-phone list UI is blocked on a future contract API decision.
- **Partial outcomes and retry scope (`src/ipc/ControlPairing.cpp`, `src/ipc/ControlRevocation.cpp`, `src/ipc/ControlService.cpp`):** `ForgetPhone(phone_id)` removes the phone from the local trust store first and then executes, **on the wireless path only**, the helper AP rotation (`RevokeAP1`) and bond demotion (`RevokePeerV1` / reconciliation).
  - **The background refresh loop does not retry AP rotation.** `ControlService::refresh_consumer_owner` skips every pending revocation record with a nonzero phone id (`if (record.snapshot.phone || record.next_attempt > attempt_now) continue;`) — that loop only retires source records. An AP leg that failed stays pending until the consumer calls `ForgetPhone(phone_id)` again: `revoke_phone` finds the existing record for that phone and runs `attempt_revocation` on it directly. Do not assume "eventually consistent" AP rotation without that explicit retry.
  - **Bond reconciliation can settle the bond leg by itself.** A successful `ReconcileBondsV1` cycle marks a pending bond leg complete once the store no longer approves that identity (the helper demoted the bond and keeps it refused, `#136`), so it clears without a `ForgetPhone` retry and emits `phone_revocation_external_complete`. Only the AP leg (and source retirement) needs the explicit retry.
  - **Transport scope:** the wired receiver starts its control service without a helper confirmation backend (`ControlBusService::Deps` with `confirmation == nullptr`). There is no AP to rotate and no BlueZ bond to demote on that path, so `ForgetPhone` on `aa-wired-receiver` is a local-store revocation only: a pending bond leg reports `helper_unavailable`, and the AP leg is treated as complete without helper work. Treat the AP/bond description in this section as wireless-only.
  - **Reporting:** local revocation can land before the helper legs complete, and `ForgetPhone` can return an error (e.g., `org.freedesktop.DBus.Error.Failed` on helper or non-durable store failure) while those external effects remain tracked as pending. Never announce a completed forget from the method's return alone; `phone_revocation_ap_pending` / `phone_revocation_bond_pending` / `phone_revocation_external_complete` are the tracked states.

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

Custom contract errors defined in `src/ipc/dbus_adapter/DispatchReply.hpp`:

| Error | Meaning |
| --- | --- |
| `org.custom.AndroidAutoReceiver1.Error.UnsupportedContractMajor` | Caller addressed an unsupported `org.custom.AndroidAutoReceiverN` major version. |
| `org.custom.AndroidAutoReceiver1.Error.PeerUnauthorized` | Caller UID mismatch, unowned consumer name, or not the active consumer. |
| `org.custom.AndroidAutoReceiver1.Error.ConsumerRejected` | Another consumer is already registered, no consumer is registered, or pairing request/window already open or spent. |
| `org.custom.AndroidAutoReceiver1.Error.MalformedRequest` | Zero or unknown `request_id` in `PairingGate` (`src/ipc/PairingGate.cpp`). Not used for D-Bus signature or viewport-bounds errors. |
| `org.custom.AndroidAutoReceiver1.Error.QueueFull` | Pending pairing table full (`8`), tracked revocation queue full (`256`), or gate-lifetime request budget spent (`64`; restart required). |
| `org.custom.AndroidAutoReceiver1.Error.IllegalTransition` | Invalid state transition (e.g., `StartProjection` while projecting, `RequestIdrRecovery` while stopped or with no active session). |
| `org.custom.AndroidAutoReceiver1.Error.NotWiredCandidate` | `GetPairingCandidate` called on a request with no wired candidate (e.g., wireless request). Request stays pending. |
| `org.custom.AndroidAutoReceiver1.Error.NotDurable` | Wired `ConfirmPhonePairing` wrote the record, but directory `fsync` failed after bounded retry. Request is spent; do not report approved. |

### Wire Dispatch Errors

Standard D-Bus errors returned by `src/ipc/dbus_adapter/ControlDispatcher.cpp` before or around method execution. The table covers the **receiver-interface methods** (`org.custom.AndroidAutoReceiver1`); the `Properties` and `Introspectable` standard interfaces have their own behaviour, listed after it.

| Error | When returned |
| --- | --- |
| `org.freedesktop.DBus.Error.InvalidArgs` | Wrong D-Bus method signature, unparseable or trailing body bytes, invalid boolean value (`> 1`), `ForgetPhone(0)`, or out-of-range `SetDisplayViewport` bounds (`width`/`height` not in `1..8192` or `dpi` not in `1..2000`). |
| `org.freedesktop.DBus.Error.Failed` | Missing daemon-stamped unique sender name, or any trust/helper/internal error without a custom mapping above (including `trust_unknown_phone`, `trust_rejected`, `helper_rejected`, `helper_unavailable`, and `trust_store_not_durable` outside `ConfirmPhonePairing`). |
| `org.freedesktop.DBus.Error.UnknownObject` | Object path is not `/org/custom/AndroidAutoReceiver`. |
| `org.freedesktop.DBus.Error.UnknownInterface` | Interface is not `org.custom.AndroidAutoReceiver*`, `Properties`, or `Introspectable`. |
| `org.freedesktop.DBus.Error.UnknownMethod` | Unknown member name on the interface. |

Standard-interface behaviour (`src/ipc/dbus_adapter/PropertiesDispatch.cpp`):

- `Properties.Get` / `GetAll` return `ContractSemVer`; an unknown property name returns `org.freedesktop.DBus.Error.UnknownProperty`, an unknown or foreign interface returns `UnknownInterface`, and malformed arguments return `InvalidArgs`.
- `Properties.Set` never writes: `Set` of `ContractSemVer` returns `org.freedesktop.DBus.Error.PropertyReadOnly`, an unknown property name returns `UnknownProperty`, and a malformed body returns `InvalidArgs`.
- `Introspectable.Introspect` returns the receiver's XML. A call with the right member name but an unexpected signature or a non-empty body returns `org.freedesktop.DBus.Error.UnknownMethod` (not `InvalidArgs`), and any other member on that interface returns `UnknownMethod` too.

---

## 3. AF_UNIX Video and Input Framing ABI and Display Geometry

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

#### Video Limits Record Layout

Sent by both sides immediately after `SO_PEERCRED` passes (32 bytes, big-endian). Effective limits are per-field `min(local, peer)`; handshake failure closes the connection.

| Field | Offset | Size | Description |
| --- | --- | --- | --- |
| `magic` | `0` | `4` | `0x41414C31` (`"AAL1"`). |
| `version` | `4` | `2` | Protocol version (`u16 = 1`). |
| `record_size` | `6` | `2` | Record size (`u16 = 32`). |
| `kind` | `8` | `4` | Limits-exchange record kind (`u32 = 1`). |
| `max_datagram_payload` | `12` | `4` | Fragment payload cap (`u32`, `1024..16384`). |
| `max_access_unit_bytes` | `16` | `4` | Reassembled AU cap (`u32`, `1..4194304`). |
| `socket_budget_bytes` | `20` | `4` | Kernel socket buffer budget (`u32`, `1..262144`). |
| `reserved_0` | `24` | `4` | Reserved (`u32 = 0`). |
| `reserved_1` | `28` | `4` | Reserved (`u32 = 0`). |

#### Video Fragment Header Layout

56-byte big-endian header preceding `payload_bytes` of encoded H.264/H.265/VP9/AV1 data. Slow consumers lose data at access-unit granularity with drop-to-IDR resync; end of stream is `shutdown(SHUT_WR)`, never a zero-length record.

| Field | Offset | Size | Description |
| --- | --- | --- | --- |
| `magic` | `0` | `4` | `0x41415631` (`"AAV1"`). |
| `version` | `4` | `2` | `u16`: `1` without profile metadata, `2` with profile metadata. |
| `header_size` | `6` | `2` | Fixed header size (`u16 = 56`). |
| `idr` | `8` | `4` | `u32`: `0` non-IDR, `1` IDR. |
| `frame` | `12` | `8` | `u64` access-unit id. |
| `index` | `20` | `4` | `u32` fragment index (`0..count-1`). |
| `count` | `24` | `4` | `u32` total fragments for this access unit. |
| `sequence` | `28` | `8` | `u64` monotonic per-fragment wire sequence. |
| `timestamp_ns` | `36` | `8` | `u64` common monotonic A/V clock. |
| `frame_bytes` | `44` | `4` | `u32` full access-unit byte length. |
| `payload_bytes` | `48` | `4` | `u32` fragment payload byte length. |
| `metadata` | `52` | `4` | `u32`: `0` for version `1`; `(resolution_index << 16) \| (fps << 8) \| codec` for version `2` (`resolution_index` 1-based into `kVideoResolutions` `1..9`, `fps` `30` or `60`, `codec` `3`=h264, `5`=vp9, `6`=av1, `7`=h265). |

### Input Socket ABI

- **Path:** `android-auto-receiver/input.sock` (`include/aa/ipc/InputSocket.hpp`, `include/aa/ipc/InputChannel.hpp`, `src/ipc/input/InputChannel.cpp`, `src/ipc/input/InputCodec.cpp`)
- **Direction:** Receiver binds/listens (`InputServer` / `InputEndpoint`); infotainment consumer connects nonblocking and sends (`InputSender`).

#### Input Framing Constants

| Constant | Value | Meaning |
| --- | --- | --- |
| `kInputRecordBytes` | `24` | Fixed big-endian input datagram size in bytes. |
| `kInputProtocolVersion` | `1` | Input wire protocol version. |
| `kInputRecordMagic` | `0x41414931` | `"AAI1"` big-endian magic at offset 0. |
| `kInputKindKey` | `1` | Keyboard / D-pad / steering-wheel Android keycode event. |
| `kInputKindTouch` | `2` | Absolute-viewport touch event. |

#### Input Record Layout

24 bytes, big-endian. `InputSender` is strictly nonblocking so a congested receiver (`transport_queue_full`) never stalls the consumer's GUI thread; congestion is counted without tearing down the socket.

| Field | Offset | Size | Description |
| --- | --- | --- | --- |
| `magic` | `0` | `4` | `0x41414931` (`"AAI1"`). |
| `version` | `4` | `2` | Input wire protocol version (`u16 = 1`). |
| `record_size` | `6` | `2` | Fixed record size (`u16 = 24`). |
| `kind` | `8` | `4` | `u32`: `1` key, `2` touch. |
| `a` | `12` | `4` | `u32`: Android `keycode` for key; `x` in input-descriptor pixels for touch. |
| `b` | `16` | `4` | `u32`: `pressed` (`0\|1`) for key; `y` in input-descriptor pixels for touch. |
| `c` | `20` | `4` | `u32`: `0` for key; `TouchAction` (`0`=down, `1`=move, `2`=up) for touch. |

### Display Geometry and Input Mapping

- **No geometry negotiation via `SetDisplayViewport`:** `SetDisplayViewport(width, height, dpi)` only validates (`width`, `height` in `1..8192`; `dpi` in `1..2000`) and stores the consumer-supplied viewport (`src/ipc/ControlService.cpp`). The receiver does **not** renegotiate video profiles or update input clamping from it.
- **Stream profiles:** The receiver offers an ordered list of `VideoProfile` entries (`include/aa/config/VideoProfiles.hpp`, `docs/video-pipeline.md`), defaulting to `1280x720@30:h264, 800x480@30:h264` with `1280x720@30:h264` as the universal fallback unless `--exact-video-profiles` is requested. Supported wire resolutions (`include/aa/protocol/Protocol.hpp`): five landscape sizes (`800x480`, `1280x720`, `1920x1080`, `2560x1440`, `3840x2160`) and four portrait sizes (`720x1280`, `1080x1920`, `1440x2560`, `2160x3840`); `480x800` is excluded.
- **Screen insets and scaling:** `aa::config::Screen` (`include/aa/config/Screen.hpp`, default `density = 160`) defines pixel margins and content insets in primary-profile pixels and scales them per axis (`scale_screen`) to every offered profile.
- **Touch limits and coordinate mapping:**
  - Both shipped receiver entry points advertise only `ButtonConfiguration` on the AAP input channel (`tools/wired-receiver/main.cpp`, `tools/wireless-receiver/ServiceDescription.hpp`), not a touchscreen descriptor.
  - `aa::input::InputService` (`src/input/InputService.cpp`) clamps incoming `TouchEvent` coordinates against a separate `aa::input::CapabilityDescriptor::touch_viewport()` defaulting to `1280x720` (`src/input/Capability.cpp`) — not `SetDisplayViewport` and not automatically the negotiated video stream dimensions.
  - Consumers must not assume consumer viewport, decoded video stream dimensions, and input clamp dimensions are identical. The reference display (`src/display/qt_adapter/ProjectionWidget.cpp`) maps surface coordinates through the letterboxed `video_rect()`: subtract `area.topLeft()` from the pointer position, scale `(local.x * input_width / area.width, local.y * input_height / area.height)`, and clamp to `[0, input_width - 1]` x `[0, input_height - 1]`.

---

## 4. Consumer-Helper Usage (`libaareceiver-consumer`)

Infotainment's C++ AA adapter should link `libaareceiver-consumer` (`include/aa/consumer/Decode.hpp`) to decode `aa::ipc::VideoAccessUnit` payloads received from `VideoConsumer`:

1. **Build/integration path:** The Debian package (`android-auto-receiver`) installs runtime binaries and `libaareceiver-consumer.a`, but ships no public headers (under `include/aa/`), no contract doc, and no IPC archives (`VideoConsumer`, `InputSender`). Until a dedicated `-dev`/SDK package is published, the supported integration path for an independent C++ consumer is to build and link `libaareceiver-consumer` and the IPC libraries directly from a pinned `zucch1/android-auto-jetson` source checkout.
2. **Probe (optional, safe before open):** Call `DecodeHelper::probe_backends(codec)` to inspect runtime GStreamer element viability (`nvv4l2decoder` + `nvvidconv`, software decoders). Presence is measured at runtime, never assumed from JetPack.
3. **Open and profile selection (`src/display/DisplayController.cpp`):**
   - `DecodeHelper::open(DecodeConfig{caps, backend, on_frame, on_completion})` opens for fixed `DecodeConfig::caps`; an observed output dimension mismatch after `open()` is terminal (`channel_caps_unsupported`).
   - `DecodeBackend::automatic` selects NVIDIA (`nvv4l2decoder` + `nvvidconv`) when the complete output path is viable, otherwise falls back to software at `open()` time only. `DecodeBackend::nvidia` never silently downgrades; it fails typed (`channel_decode_unavailable`).
   - Open initially with the consumer's configured `caps` (used for profile-less v1 streams) and size the video socket receive buffer for those caps (`recommended_socket_buffer_bytes`).
4. **Stream loop (single-threaded, caller-driven):**
   - Drive `VideoConsumer::receive()` until it returns `ReceiveStatus::complete` before reading `access_unit()` and submitting.
   - **Profile change / fallback reopen:** When a v2 access unit carries `unit.profile` differing from the active `caps`, require `unit.idr` (otherwise treat as `channel_caps_unsupported`), open a new `DecodeHelper` with `caps = *unit.profile`, close the old helper, update active caps, and resize the socket receive buffer before submitting `unit`.
   - Submit reassembled access units with `submit(au)`. `timestamp_ns` must be strictly increasing and session-unique (no reset on IDR recovery).
   - Drain decoded I420 frames with `pump(wait)`. `on_frame(const DecodedFrame&)` executes on the `pump()` caller's thread, never on a GStreamer streaming thread. Each `DecodedFrame` carries tightly packed I420 (`Y + U + V`) and the original `frame`, `sequence`, `timestamp_ns`, `width`, `height`, and `fps`.
5. **Error and resync handling — two independent loss paths:**
   - **`ipc_queue_full` from `submit()` (retryable backpressure):** the byte backlog rejected the AU and its timestamp was not consumed. Keep **exactly one** pending AU: copy it (`unit = video.access_unit()`) before any further `receive()`, do not read ahead, then drain via `pump()` and retry the *same* AU before reading or submitting any later unit (`src/display/DisplayController.cpp`: `pending_unit`, retried at the top of `pump()`; a repeat `ipc_queue_full` keeps it and returns to draining). Reading or submitting a later AU first would make the retained unit invalid — `timestamp_ns` must stay strictly increasing (item 4), so the later timestamp permanently poisons the retry. Drop the pending AU only when the retry fails with a non-retryable code.
   - **`ipc_consumer_rejected` from `submit()` or `pump()` (decoder loss):** non-terminal loss notice (5-second operational retention bound, capacity saturation, or accepted AUs stranded during `end_of_stream()` as `frames_lost_eos`). **Check `report().awaiting_idr` before triggering recovery:** `pump()` can report an older loss episode even after a recovery IDR has already been accepted (`awaiting_idr == false`) or during EOS drain. Only discard non-IDRs and call `RequestIdrRecovery` over D-Bus when `report().awaiting_idr` is `true`.
   - **Socket-side loss (before the decoder ever sees the AU):** `VideoConsumer` runs its own drop-to-IDR reassembly state and **suppresses** non-IDR units while it is waiting, so `receive()` returns `ReceiveStatus::waiting` and the units never reach `submit()`. Observe `video.waiting_idr()` alongside `report().awaiting_idr`: both conditions need the same recovery request. The socket state is `true` from connection open until the first IDR, so gate it with a startup grace — only treat it as loss after a unit was seen, after `rejected() > 0` or `suppressed() > 0`, or after ~1 s of projection (`kIdrStartupGrace`); otherwise a fresh stream fires a request before any video has flowed. Model on `DisplayController::observe_idr_recovery()`.
   - **Bounded, rate-limited requests:** drive both paths through one coordinator, not a raw `RequestIdrRecovery` per error. Requests are at most one per 250 ms (`kIdrRequestMinInterval`), the episode escalates after 1 s to a bounded number of nudges/transitions (`kIdrEscalationDelay`, `kIdrEscalationMaxNudges = 3`, 5 s transition cooldown), and I/O failures back off to a 2 s cap (`kIdrFailureBackoffCap`) while an idle link (`session_illegal_transition`) keeps the normal cadence. Keep the episode open and requesting while **either** decoder `report().awaiting_idr` **or** the startup-gated socket `waiting_idr()` is true; close it only when **both** are false (`DisplayController::observe_idr_recovery()`). Stopping when only one clears can strand a socket-only loss while the decoder is healthy.
   - `channel_decode_failed` or `channel_caps_unsupported` after `open()`: terminal for that `DecodeHelper` instance. Close, reopen, and request a fresh IDR.
6. **Teardown:** Call `end_of_stream()`, drain via `pump()`, and `close()` (idempotent; destructor also closes).

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

### User vs System Boundaries and Packaged Defaults

- **System unit (`/usr/lib/systemd/system/aa-system-helper.service`):** Runs as `root` with `aa-system-helper.service`, the system D-Bus policy + per-UID delivery fragment, `/etc/android-auto-receiver/session.uid`, `/etc/android-auto-receiver/receiver.sha256`, `/etc/android-auto-receiver/ap.conf`, and `70-android-auto-receiver.rules`.
- **Packaged user unit (`/usr/lib/systemd/user/aa-receiver.service`, PR `#205` / `#189`):** Runs in the user session (`PartOf=graphical-session.target`, `WantedBy=graphical-session.target`, `LimitCORE=0`) and starts `/usr/bin/aa-wireless-receiver --store-dir %S/android-auto-receiver --pairing-only`.
- **Operator activation and projection-mode override:**
  - Package maintainer scripts never enable or start either `aa-system-helper.service` or `aa-receiver.service`; operators must enable and start them explicitly.
  - The packaged `--pairing-only` default **cannot project** (`tools/wireless-receiver/Options.hpp`). Presence of `org.custom.AndroidAutoReceiver` on the session D-Bus therefore does **not** imply projection readiness — it may only be serving enrolment/control in `--pairing-only` mode.
  - To enable projection, the operator must provision `/etc/android-auto-receiver/session.uid`, `/etc/android-auto-receiver/receiver.sha256` (computed from the installed receiver binary after each install/upgrade), `/etc/android-auto-receiver/ap.conf`, the per-UID D-Bus fragment, and `HU_KEY_PATH` (an owner-only certificate/private-key PEM bundle; see `tools/wireless-receiver/Options.hpp`, `docs/tls-credentials.md`), and install a per-user systemd drop-in overriding `ExecStart` to run with `--phone-id <N>` (or `aa-wired-receiver`) instead of `--pairing-only`. Any custom receiver unit must preserve `LimitCORE=0`.
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
| Authority XML | `docs/dbus/org.custom.AndroidAutoSystemHelper1.xml` (reference metadata only; helper does not expose `Introspectable`) |

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

Normative replacement rules live in `docs/system-helper.md` (Section "System bus API 1.5.0") and `src/helper/SystemHelper.cpp`. Any replacement must preserve:

- **Errors, states, and no `Introspectable`:** Wire error codes `0` (success), `1000` (unavailable), `1001` (rejected), `1002` (protocol mismatch). States: `0` (inactive), `1` (active), `2` (unavailable). Unknown interfaces, paths, members, or signatures fail closed. `docs/dbus/org.custom.AndroidAutoSystemHelper1.xml` is reference metadata only; the helper does not expose `org.freedesktop.DBus.Introspectable`.
- **Single receiver identity pinning:** Every call authenticates the daemon-stamped unique sender via `GetConnectionUnixUser`, `GetConnectionUnixProcessID`, `/proc/PID/status` UIDs, live starttime, and full SHA-256 of `/proc/PID/exe` against `/etc/android-auto-receiver/session.uid` and `/etc/android-auto-receiver/receiver.sha256`. The first authenticated receiver identity is pinned for the lifetime of the helper run; any other caller is rejected (`src/helper/SystemHelper.cpp`).
- **Grant-consuming vs grant-zero actions:** Only privileged actions `1` (`acquire_ap`, channel `36`/`40`/`44`/`48` matching `ap.conf`) and `2` (`open_pairing`, channel `0`) require and consume a single-use `Issue` grant (consumed before revalidation and backend side effects). All non-privileged actions (`3` `release_ap`, `4..6` status, `7`/`8` profile register/unregister, `10` `close_pairing`) must pass `grant = 0` and fail closed if a nonzero grant is supplied (`src/helper/SystemHelper.cpp`). Action `9` is reserved and rejected.
- **First-`ReconcileBondsV1` admission gate and packed result:** Until the first successful `ReconcileBondsV1` of a helper run, nothing is admitted, no enrolment opens, and no journal entry is released (`docs/system-helper.md`). Its `t` return value packs the unadmitted count in the low 32 bits and the open-window flag in bit 32.
- **Grant and bond invariants:** Max `16` pending grants, `4096` lifetime replay history, `5 s` grant TTL, non-DFS UNII-1 AP channels (`36`, `40`, `44`, `48`), durable `managed-bonds` / `pending-removals` / `revoked-bonds` under `/var/lib/aa-system-helper` (`0700`).
- **BlueZ restraint:** Never call `Connect`, `ConnectProfile`, `Disconnect`, `DisconnectProfile`, `CancelPairing`, or `RemoveDevice` (`#136`, `#160`).

---

## 7. Audio Streams and Focus Requests

- **Advertised production endpoints vs mixer capabilities:**
  - **Currently advertised endpoints (`tools/wired-receiver/main.cpp`, `tools/wireless-receiver/ServiceDescription.hpp`):** Both production receiver entry points advertise three playback endpoints — `media_audio` (`48000 Hz`, 16-bit, 2 ch), `guidance_audio` (`16000 Hz`, 16-bit, 1 ch), and `system_audio` (`16000 Hz`, 16-bit, 1 ch) — plus a separate `microphone` capture endpoint (`16000 Hz`, 16-bit, 1 ch). Neither entry point advertises a `speech_audio` playback endpoint.
  - **Internal mixer capabilities (`include/aa/audio/Audio.hpp`, `include/aa/audio/AudioMix.hpp`, `include/aa/audio/AudioRouter.hpp`):** The audio router can decode four AAP roles (`media`, `guidance`, `system`, `speech`; PCM or AAC-LC ADTS), duck and mix them internally (`media` `1.0` / `0.25` (-12 dB) ducked by `guidance`/`speech` within a `200 ms` window; `guidance` `1.0`; `system` `0.5` (-6 dB); `speech` `1.0`), and emit one mixed `48000 Hz`, 2-channel, 16-bit PCM stream.
- **Audio backend:** AA routes through GStreamer `pulsesink` (`pipewire-pulse`) to the current default PipeWire output. Native `pipewiresink` is refused on PipeWire 1.0.5 / Ubuntu noble (`#80`).
- **Focus requests:** Inside the AAP protocol (`include/aa/protocol/Messages.hpp`), the phone sends `AudioFocusRequest` (`ControlMessageId::audio_focus_request = 18`: `gain`, `gain_transient`, `gain_transient_may_duck`, `release`) and AA replies with `AudioFocusNotification` (`ControlMessageId::audio_focus_notification = 19`). Currently `src/app/WiredSession.cpp` answers focus requests locally (`release -> loss`, otherwise `gain`), and `AudioRouter::set_muted()` provides global mute. No external focus forwarding RPC exists in receiver contract `1.1.0`.
- **Infotainment arbitration and transport difference for `StopProjection`:**
  - Infotainment owns system-wide audio focus, volume, and output routing, and arbitrates AA audio at the PipeWire/WirePlumber layer (mute, duck, cork, or route AA's `pulsesink` stream).
  - **`StopProjection` does not silence wired AA:** `aa-wireless-receiver` (`tools/wireless-receiver/Runtime.hpp`) observes D-Bus control state and stops the receiver session on `StopProjection`, whereas `aa-wired-receiver` (`tools/wired-receiver/main.cpp`) continues servicing its active session, audio, and input independently of `StopProjection` control state. Infotainment must therefore apply its PipeWire/WirePlumber routing/mute policy directly to the actual AA audio stream whenever wired audio must yield.

---

## 8. HFP Precondition

- Infotainment owns the Bluetooth HFP Hands-Free link (`0000111e`, WirePlumber on RFCOMM channel 7).
- A bonded phone initiates the wireless AA RFCOMM bootstrap (`4de17a00-52cb-11e6-bdf4-0800200c9a66`) only over an existing Bluetooth link (`docs/wireless-handoff.md`, `#160`).
- AA never pages, connects, or disconnects HFP. During a reconnect window, AA waits for the phone to dial.
- If the attempt deadline expires with no live bootstrap, `aa-wireless-receiver` emits the structured receiver log event `wireless_admission_expired` (`tools/wireless-receiver/Runtime.hpp`, `tools/wireless-receiver/AdmissionEvents.hpp`) — not a D-Bus signal — with one of three phases:
  - `phase: ap_activation`, `reason: attempt_deadline` (still waiting for AP activation).
  - `phase: bluetooth_link`, `reason: no_bluetooth_link`, `detail: "waiting for HFP (owned by infotainment)"` (when `GetBootstrapWaitV1` reports `Device1.Connected` false for the admitted phone).
  - `phase: phone_handoff`, `reason: attempt_deadline` (fallback phase when the link is connected, the helper is `1.4`, or the wait query timed out). Note that `Device1.Connected` being true by itself does **not** prove HFP is active.
- Forgetting a phone in AA (`ForgetPhone`) revokes AA trust and, on the wireless path, moves the bond to `revoked-bonds`, but leaves the BlueZ pairing intact (`#136`). To re-enrol a forgotten phone in AA, the user unpairs it in infotainment's Bluetooth settings first, then enrols it again. (The wired control service has no helper backend, so it only revokes AA trust locally.)

---

## 9. Privacy Rules

The following data must **never** be logged, exported in diagnostics/evidence, broadcast over D-Bus signals (`DiagnosticEvent`), or committed to git (`tools/qualification/check_privacy.py`, `docs/tls-credentials.md`, `docs/system-helper.md`):

- **Credentials and keys:** Head-unit TLS private keys and certificates (`headunit.key`, `headunit.crt`, PEM blocks), auth tokens, and bearer credentials.
- **Wi-Fi AP secrets:** AP SSID, WPA2 PSK, BSSID, and `/var/lib/aa-system-helper/ap.credentials` contents. AP credentials are confined strictly to the authenticated helper/receiver handoff (`GetAPCredentialsV1`) and the wireless bootstrap; keep them out of consumer telemetry and evidence. The helper holds PSKs in `explicit_bzero`-wiped fixed buffers (`nm::Psk`), enforces `LimitCORE=0`, and strips `G_DBUS_DEBUG`.
- **Bluetooth secrets and addresses:** Link keys, raw Bluetooth MAC addresses, and raw adapter addresses in logs, telemetry, or broadcast events. Raw comparison identity fields (`address`, `adapter`, and the 6-digit `comparison` from `GetPairingComparison`) are for the active registered consumer only during explicit enrolment and must be kept out of consumer logs, telemetry, and evidence exports.
- **Trust stores:** Contents of `approved-phones.json`, `managed-bonds`, `pending-removals`, and `revoked-bonds`.
- **Phone and location identifiers:** USB serial numbers, AOA strings, plaintext USB port strings (`tools/wired-receiver/main.cpp` redacts USB ports via `aa::diagnostics::redact_identifier` in startup and refusal logs), IMEI, IMSI, Android ID, MEID, UDID, and raw GNSS/location coordinates.
- **Raw captures:** Packet or media dumps (`.pcap`, `.pcapng`, `.h264`, `.aac`, `.pcm`, `usbmon`, `snoop`).

**Enforcement scope:** `tools/qualification/check_privacy.py` is a conservative export-time check that requires explicit manual review (`known export checks only; manual review required`), not a universal secret detector or runtime anonymizer.

Permitted telemetry: opaque per-process pseudonyms (`label` from `GetPairingCandidate`, `aa::diagnostics::redact_identifier` values), numeric `PhoneId`/`request_id`/`subject_id`, and consumer-only (never broadcast) responses to `GetPairingComparison` and `GetPairingCandidate` during active explicit enrolment.

---

## 10. Infotainment Handoff Checklist

Exact amendments required in `zucch1/infotainment` when integrating Android Auto (none are implemented by this repository):

### Settings

- [ ] Add an Android Auto settings page behind `AppState` (QML declarative only): show AA availability/degraded state when `org.custom.AndroidAutoReceiver` is absent (and note that bus-name presence alone does not prove projection readiness if the unit is running `--pairing-only`), and drive explicit enrolment separately for wireless (exactly one visible public BR/EDR candidate established *first*, then `RequestAddPhone` -> `ConfirmPhonePairing` to open window / `PairingOpened` -> poll `GetPairingComparison` -> human modal -> `RespondPairingComparison(request_id, challenge_id, accept)`; a non-durable approval is retracted best-effort) and wired (refused candidate under `--wired-enrolment` -> `RequestAddPhone` -> `GetPairingCandidate` -> human modal -> `ConfirmPhonePairing` / `CancelPhonePairing` -> replug/restart; after a durable confirm go straight to replug/restart, never a post-confirm `CancelPhonePairing`).
- [ ] Approved-phone list / forget UI: **blocked on an API decision** — `org.custom.AndroidAutoReceiver1` (`1.1.0`) provides `ForgetPhone(t)` with no list API, and `GetPairingCandidate` exposes no persistent `phone_id`. Do **not** read `approved-phones.json` directly. When `ForgetPhone` is wired: handle partial outcomes (local revocation can land while helper AP/bond legs remain pending and return an error), never announce a completed forget from the call's return, and retry the whole `ForgetPhone(phone_id)` explicitly for a pending AP leg — the receiver's refresh loop does not, while bond reconciliation may settle its own leg. The AP/BlueZ legs exist only on the wireless receiver.
- [ ] Keep Bluetooth/HFP pairing and unpairing under infotainment Settings so the owner can connect HFP (`#160`) and unpair a forgotten phone before re-enrolment (`#136`).
- [ ] Feature-detect receiver contract `1.1.0` via `ContractSemVer` / `GetCapabilities` before calling `GetPairingCandidate`, and handle `Error.NotDurable` by never reporting an unconfirmed wired approval as succeeded.

### App-Host

- [ ] Implement a C++ AA consumer adapter behind `AppState` (built against a pinned `zucch1/android-auto-jetson` source checkout until an SDK package exists) that registers as the exclusive session consumer (`RegisterConsumer`), sends `SetDisplayViewport(width, height, dpi)`, connects to `video.sock` (`aa::ipc::connect_video_consumer`), decodes frames via `aa::consumer::DecodeHelper` (including v2 profile-change reopen at IDR and rate-limited recovery triggered by either `report().awaiting_idr` or the socket's startup-gated `waiting_idr()`, ~1 s grace, closed only when both clear, retaining one queue-full AU for retry), renders I420 frames into the app-host surface, and maps pointer coordinates through the letterboxed video rect into the input descriptor's coordinate space over `input.sock` (`aa::ipc::InputSender`).
- [ ] Support the selected display runtime (`zucch1/infotainment#9`: Weston `ivi-shell` client surface or Qt Wayland Compositor / embedded QML texture under `eglfs`) without placing D-Bus or socket I/O in QML.
- [ ] Wire `StartProjection` / `StopProjection` to app foreground/background transitions and request an IDR on surface resume, or on socket `waiting_idr()` / `ipc_consumer_rejected` with `report().awaiting_idr` true — through one rate-limited coordinator, never one raw call per notice.
- [ ] Arbitrate AA's `pulsesink` PipeWire stream for volume, mute, ducking against infotainment navigation/media, and output routing; apply routing/mute directly to the PipeWire stream when wired AA must yield because `StopProjection` does not silence `aa-wired-receiver`.

### Startup

- [ ] Keep `infotainmentd`, `infotainment-ui`, and display-runtime units completely independent of `aa-system-helper.service` and `aa-receiver.service` (no `Requires=`, `Wants=`, or `After=` on AA).
- [ ] Watch `org.custom.AndroidAutoReceiver` name ownership on the session bus: attach lazily when it appears, re-register after receiver restarts, degrade cleanly on vanish, and rate-limit retries to at most once per second.
- [ ] Bring up HFP (`0000111e` via WirePlumber) for bonded phones so wireless AA reconnect windows can complete without AA paging Bluetooth.

### Guard Amendments

- [ ] `tests/scope/check_network_listeners.py` (in `zucch1/infotainment`): scans `src/` and `qml/` for server APIs and POSIX `bind`/`listen` calls (allowing CAN-only sockets and `127.0.0.1` loopback routing). AA's consumer adapter only calls `connect()` on `AF_UNIX` (`video.sock`, `input.sock`) and requires no TCP/UDP listener. Amend this guard **only if** infotainment adds an `AF_UNIX`-only Qt Wayland Compositor (`bind`/`listen` on a Wayland socket) — explicitly allowlist `AF_UNIX`-only sockets without `AF_INET`/`AF_INET6`, never weakening the `127.0.0.1` loopback-only rule for IP listeners.
- [ ] `tests/smoke/check_daemon_no_network.sh` (in `zucch1/infotainment`): launches `infotainmentd` and probes listening TCP/UDP sockets via `ss` and `/proc/net/{tcp,tcp6,udp,udp6}`. Keep `infotainmentd` free of listening TCP/UDP sockets; AA's AP/TCP listener stays in `aa-wireless-receiver` / `aa-system-helper`, never in `infotainmentd`.
- [ ] `tests/navigation/check_qt_purity.py` and `tests/smoke/check_ui_daemon_contract_seam.cmake` (in `zucch1/infotainment`): `check_qt_purity.py` checks that `CMakeCache.txt` and built ELF binaries link only against the pinned Qt prefix (`/opt/Qt/6.8.3/gcc_aarch64`) rather than system Qt; `check_ui_daemon_contract_seam.cmake` runs `infotainmentd --smoke` (checking the `infotainmentd smoke ok; contract 1.0.0` marker) and `infotainment-ui --smoke` offscreen. Neither enforces the QML/C++ adapter boundary. Keep all AA D-Bus, socket, and GStreamer decode calls inside the C++ adapter behind `AppState`, and add a dedicated boundary check if automated QML-isolation enforcement is desired.
- [ ] `tests/scaffold/check_repository_layout.cmake` (in `zucch1/infotainment`): checks that a fixed `required_paths` list exists in the repository (not a directory registration mechanism). Only add an AA adapter path there if you want its presence enforced as a required scaffold path.
- [ ] Provisioning hygiene: Provision `/etc/android-auto-receiver/session.uid`, `/etc/android-auto-receiver/receiver.sha256`, `/etc/android-auto-receiver/ap.conf`, `HU_KEY_PATH`, and the per-UID D-Bus delivery fragment via AA packaging/deployment; never commit them to either repository.
