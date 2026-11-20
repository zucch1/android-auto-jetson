# Wireless handoff, projection and recovery (host reference)

Task37 implements the receiver listener, helper-owned RFCOMM bootstrap, and
shared projection-session path for host testing; task38 adds the host
reconnect/recovery matrix. This is a reduced-scope result: scripted-peer
success does not qualify a real phone; real-device results are recorded
separately. TCP endpoint binding is described below.

## Two protocol surfaces

The Bluetooth AAW bootstrap and the negotiated AAP Wi-Fi service are distinct.
They have different schemas, enum values, timing, and security boundaries.

Separately, WirePlumber owns the Jetson's HFP Hands-Free unit (`0000111e`) on RFCOMM channel 7
for Android Auto head-unit presence. The helper does not register a competing
provider or own its socket; it authorizes that existing service only for the same
exact approved and bonded peer. RFCOMM acceptance alone is not HFP success: the
phone and WirePlumber must complete service-level negotiation. No call-control
or audio/SCO behavior is added by this authorization rule.

### Helper-owned RFCOMM AAW bootstrap

`aa-system-helper` owns the AA profile socket, record framing, and bootstrap
state machine for UUID `4de17a00-52cb-11e6-bdf4-0800200c9a66`. The receiver
never receives a generic RFCOMM byte stream or an arbitrary socket FD.

Each record has a four-byte header followed by its protobuf payload. The first
u16 is the big-endian payload length, excluding the header; the second u16 is
the big-endian message ID. Payloads larger than 4096 bytes are protocol
violations. Fragmented records are accumulated and coalesced records are split.

The receiver plays the head-unit role and runs the start-first flow (#166) that
both shipping head-unit-role dongles use against current phones:

```text
receiver  WifiStartRequest=1 {1: ip, 2: port}          authorized, listener ready
phone     WifiInfoRequest=2 {}
receiver  WifiInfoResponse=3 {1: ssid, 2: key, 3: bssid,
                              4: security_mode=8, 5: access_point_type=1}
phone     WifiStartResponse=7 {3: status = 0}
phone     WifiConnectStatus=6 {1: status = 0}
```

The start request is sent only after authorization and only once the
projection listener is ready. Credentials leave the helper only in record `3`,
from the pinned AP snapshot and behind the same snapshot fence and eligibility
checks described below. The two phone statuses differ:

- `WifiConnectStatus` must be exactly `0`; any other value is a phone rejection.
- `WifiStartResponse`'s status is optional, and only a negative value is a
  rejection. An absent status, `0` and positive values such as `1` are accepted
  (aa-proxy-rs reads the record without checking its status; OAA maps `1` to
  success).

`WifiConnectionRejection=10` from the phone is a rejection.

Sources for every ID, framing rule and field number (file:line at the pins
below): aa-proxy-rs `src/bluetooth.rs:409-420` (IDs), `4692-4694` and
`4716-4722` (length + ID header), `6284-6323` (`send_params`, the flow above),
`914-923` (status tags 1 and 3), `4746-4754` (status `0` is success),
`src/protos/WifiStartRequest.proto:4-7` and `src/protos/WifiInfoResponse.proto:4-28`;
WirelessAndroidAutoDongle `aawg/src/bluetoothProfiles.cpp:32-65` (flow),
`68-77` (IDs), `99-112` and `125-142` (framing), and its
`proto/WifiStartRequest.proto:4-7` and `proto/WifiInfoResponse.proto:4-28`.
open-android-auto agrees on the IDs (`oaa/wifi/WifiSetupMessageEnum.proto:34-44`),
`WifiStartRequest` (`WifiStartRequestMessage.proto:16-20`), `WifiStartResponse`
status 3 (`WifiStartResponseMessage.proto:20-24`) and `WifiConnectStatus` status 1
(`WifiConnectStatusMessage.proto:20-23`). It swaps `WifiInfoResponse` tags 2 and 3
(`WifiInfoResponseMessage.proto:13-19`); we follow the two shipping dongles.

The version-negotiation messages (`WifiVersionRequest=4`, `WifiVersionResponse=5`,
`WifiSetupInfo=11`) are not used. The pre-#166 code sent them with every field
number one too high (the phone logged "HU requested version: 0.1"), and then
never sent setup or start. Their status values are not settled (OAA marks
success as wire `1`, unverified, at `WifiVersionStatusEnum.proto:18-31`), and no
phone capture exists to check against, so that path was removed rather than
guessed. A phone record with ID 4 or 5 is a wrong-order failure.

Every handshake failure logs one line to the helper's stderr, then fails
through the ordinary deny path:

```text
system-helper: wireless_handshake_failed phase=await_info_request msg_id=99 msg=unknown reason=unknown_id detail="record header rejected"
```

`phase` is `authorization`, `await_info_request`, `await_start_response`,
`await_connect_status` or `finishing`; `msg_id` is the record ID, or `none`;
`reason` is the typed `HandoffFailure`. The line never carries the SSID, PSK,
BSSID, endpoint or any address. A completed exchange logs
`system-helper: wireless_handshake_complete flow=start_first`.
A successful `WifiConnectStatus` ends the exchange, subject to the deadline
recheck; records after it are never read, and nothing on the channel after it
affects the completed authority.

The handshake tolerates what the shipping dongles tolerate, within bounds:
pings (`8`, `9`), `WifiSetupInfo=11` and unknown IDs are ignored, at most 32 per
exchange; the next one fails it (`unknown_id`). A repeated `WifiInfoRequest`
after the response gets the identical `WifiInfoResponse` again, at most twice;
a third repeat fails the exchange (`duplicate`).

On this AAW surface, `security_mode` uses its own bitmask-style enum with
`WPA2_PERSONAL=8`; `access_point_type` is `DYNAMIC=1`. The BSSID is the current
AP interface MAC as a lowercase, colon-separated string. The project-owned
`Bootstrap.proto` is intentional: vendored AASDK imports the negotiated Wi-Fi
projection enum, which is not the AAW bootstrap enum and cannot encode this
contract correctly.

Denial, malformed or oversized input, wrong order, duplicates, too many ignored records,
EOF, timeout, and phone-side rejection close RFCOMM without a rejection payload
or further credentials. The helper reports a typed local failure without secret
material and does not mutate trust. Normal closure after confirmed success has
the separate completed-handoff semantics described under lifetimes.

The wire rules are reverse-engineered, not an official Google interoperability
specification. Pinned head-unit-role references are
`nisargjhaveri/WirelessAndroidAutoDongle` at
`ef645221ec28e87361391b9bdcf27e31ff48cb6f` and `aa-proxy/aa-proxy-rs` at
`f80236fb17bef459398fa4ae235f86a68574a873`, with `mrmees/open-android-auto` at
`61eab61c5f9968154ff1a80faa8c0a427b208479` (vendored) as a cross-check. The
flow is exercised by host tests against scripted peers with byte-exact fixtures;
that is not proof of interoperability with a real phone. The final proof is a
live retest with the test phone (Android Auto version code 180664264).

### Negotiated AAP WifiProjection/WifiCredentials service

The second surface runs inside the established encrypted AAP session. It is
advertised only on the wireless path through the existing dynamic negotiated
service ID, never through a fixed channel ordinal. Its metadata-only response
uses that service's own `WPA2_PERSONAL=5` enum value, the configured AP channel
(`--ap-channel`, default 36), and `DYNAMIC`.
It never carries the SSID or password. Between these two surfaces, only the
RFCOMM bootstrap response carries those secrets; neither service may be used to
infer the other's wire enum.

## Authorization and credential release

### Bluetooth link dependency (#160)

The phone dials the AA RFCOMM profile only over an existing Bluetooth (HFP) link,
which infotainment owns; neither the helper nor the receiver creates or tears it
down. When an attempt's deadline passes, the receiver asks the helper
`GetBootstrapWaitV1` (contract 1.5.0) for the attempt's source. The helper applies
`PollBootstrapV1`'s sender and lease/source checks, then evaluates *now* (one
bounded inventory read, no snapshot from when the window opened): an open
reconnect window, no live bootstrap, and `Device1.Connected` false for that exact
admitted phone answer `no_bluetooth_link`; anything else answers `none`. It grants
and withholds nothing.

Compatibility: `PollBootstrapV1` is unchanged, so a 1.4 receiver keeps observing
against a 1.5 helper. A 1.5 receiver against a 1.4 helper gets the unknown member
rejected and treats it as unknown (`attempt_deadline`). A separate member was
chosen over a wider `local_failure` (which a 1.4 decoder rejects as a protocol
mismatch) and over an extra reply field (which changes `utssu`, also rejected).
A fresh reconnect window also clears the previous exchange's `local_failure`.

The receiver reports an expired attempt as
`{"event":"wireless_admission_expired","reason":"no_bluetooth_link","phase":"bluetooth_link","detail":"waiting for HFP (owned by infotainment)"}`;
otherwise `reason` `attempt_deadline` with `phase` `ap_activation` (AP not yet
active) or `phone_handoff` (a link, or an unknown answer). `Device1.Connected` is
true for any bearer, so `phone_handoff` does not prove HFP was up. No address or
identity is logged.

The receiver uses only authenticated `PollBootstrapV1`,
`AuthorizeBootstrapV1`, and `GetAPCredentialsV1`. Task37 adds no helper methods
beyond those. The receiver never reads the root-only `ap.credentials` file and
does not expose returned secrets through logs, status, properties, or persisted
receiver state.

At request entry and again immediately before any secret leaves the helper,
the helper re-resolves the receiver identity using the full executable hash.
It does not cache that digest. Credential transmission remains fenced even if
an earlier check succeeded.

`GetAPCredentialsV1` returns one coherent D3 activation snapshot: activation
generation, live BSSID and AP-side receiver IP, and the provisioned projection
port. The NetworkManager owner, device, and channel are fenced through
transmission. A generation, owner, device, channel, BSSID, or endpoint change
invalidates that snapshot rather than substituting newer state.

For a new session, the D4 chain requires all of the following:

1. The exact helper-connected peer or its completed live handoff authority.
2. An approved matching identity in the receiver TrustStore.
3. A registered, live, authorized consumer.
4. A valid pinned AP activation snapshot.

The listener is ready before `AuthorizeBootstrapV1(allow=true)` can release the
bootstrap. An approved reconnect needs no new `AddPhone` transaction. Failure
of any link denies credentials without changing trust.

### Handoff source contract

`reconnect()` resets the helper AP lease to the empty source and begins
handoff with `{consumer, 0}` (approved reconnect on the existing AP).
`acquire_ap(source)` sets the lease to that confirmation-bearing source, and
only `begin(source)` with the same source may then drive handoff. Fresh-AP
recovery is one `reconnect()`, then `acquire_ap(source)`, then
`begin(source)`; the host matrix tests this order and rejects a
zero-confirmation source after acquisition.

## Lifetimes, freshness, and deadlines

An incomplete bootstrap has a 10-second budget from accept until the head
unit's `WifiInfoResponse` is fully sent. Then, once, the deadline moves to 30 s
after that send, for the phone's Wi-Fi join and its statuses, so a whole
exchange is bounded by 40 s from accept. A stall before credentials still ends at
10 s; if the 10-second deadline has already fired when the credentials finish
sending, the exchange fails instead of being extended. The 60-second pairing window
limits only acceptance of new RFCOMM connections; it is not a TTL for an
established bootstrap, completed handoff, or projection session.

### Aborts and retries (#161)

Every aborted bootstrap shuts its RFCOMM socket down (`shutdown(SHUT_RDWR)`),
because BlueZ holds its own reference and a plain `close()` sends no DISC; the
phone then sees an IO error and restarts its wireless setup. That covers denial,
every failure, the bootstrap deadline (10 s, or 40 s once the Wi-Fi join
extension applies), a window that lapses before the bootstrap
ends (the bootstrap still runs to its own deadline), and the retirement of a
completed hand-off, whether it failed before the receiver confirmed it or its
session ended later. Completion itself shuts nothing down: the channel stays as
the phone left it for the whole session. A completion never confirmed has its
socket shut down after 30 s, keeping the completed authority. Only the AA DLC closes;
HFP, owned by infotainment, is never touched, and while it is up its DLC keeps the
RFCOMM session and the ACL up.

A retry must not reset a bootstrap that is still progressing:

- The receiver does not defer its attempt (15 s) while the coordinator is
  preparing, authorizing, or authorized but unconfirmed. That hold is bounded:
  40 s from the first observation (`HandoffTiming::bootstrap`, the helper's whole
  10 s + 30 s bound, which starts earlier) plus 5 s for the phone to open the TCP
  session of a completed exchange, so at most 45 s. A phone that completes
  RFCOMM but never opens the TCP session holds the attempt that long before the
  receiver may defer it.
- The coordinator's 10-second establishment bound applies only while it waits
  for a bootstrap; one observed late runs to its own budget.
- `begin()` with the same source during such a bootstrap keeps it: no
  retirement, no denial, and no `retired_handle` that would poison its
  re-observation.
- In the helper, a `ReconnectPeerV1` for the same phone while its window is open
  is a no-op (the window opens at most once per 60 s); a lapsed window reopens
  without aborting that phone's bootstrap; and a live bound bootstrap keeps its
  binding, pinned snapshot and lease.

Successful completion transfers the exact handle, receiver/source ownership,
BlueZ owner/device identity, and pinned AP snapshot into revocable
completed-handoff authority. It survives normal RFCOMM close and the subsequent
Profile1 `RequestDisconnection` callback, and has no absolute TTL (#161 only
shuts an unconfirmed completion's RFCOMM socket down after 30 s). Before
completion, that callback still retires the pending exchange. Device1
`Connected=false` or an invalidated `Connected` value establishes link loss;
`Connected=true` cannot revive retired authority.
Failed bootstrap, matching link loss, BlueZ owner/device/bond
loss, source retirement or revocation, receiver/helper loss, AP or snapshot
invalidation, owning-link teardown, and accepted replacement retire it.
Retirement is handle/incarnation scoped, delayed old work cannot retire a
replacement, and retired handles never revive.

Initial establishment has no one-second aggregate SLA. Once listening,
admission requires bounded-freshness authority and snapshot evidence. Only a
successful current-epoch validation, timestamped conservatively at request
start, renews freshness. The implemented budget is 950 ms evidence age plus
owner scheduling and close, totaling at most one second from actual eligibility
loss. Pending work and stale positive replies do not extend the lease; late
results are epoch-fenced and cannot reopen admission. Under v2.7, the renewing
operations are refresh validation, ready authorization, and completed-bootstrap
confirmation; completed confirmation revalidates the pinned snapshot rather
than returning an unconditional no-op. With two 25 ms owner turns, each
validating operation's full client latency must be below 450 ms, and a
non-validating operation cannot sit between refreshes while authority depends
on freshness.

Accept cancellation and listener stop complete within one second. AAP
authentication, including encapsulated TLS, has a 10-second budget. Control
operations and teardown complete within five seconds against silent or
backpressured peers. An ACTIVE session has no maximum duration.

ACTIVE requires confirmed completed bootstrap. The completion-sensitive
`AuthorizeBootstrapV1` result is `allow=1, ready=0`; a still-pending bootstrap
is retired rather than activated. Consumer loss closes admission and fresh
credential release immediately. Only an already confirmed ACTIVE session keeps
the existing 10-second degraded-consumer grace, which grants no fresh
credential or admission rights.

## Projection transport

Each accepted TCP link gets a fresh `WiredSession`, cipher, parser, and routes.
The link carries the existing AAP authentication and encapsulated-TLS encrypted
frames directly over TCP. There is no outer TLS transport. The listener binds
only the numeric IP from the pinned AP snapshot and the provisioned port, never
all interfaces.

## TCP endpoint binding

Each AP activation is bound to one phone:

- Every activation, including a rekey, starts with a fresh random SSID and
  WPA2 PSK, so credentials released for an earlier activation never admit a
  station to a later one.
- The helper binds an activation's generation to the BlueZ identity of the
  first handoff whose credentials pass the transmit fence. Credentials are
  sent only over that phone's bonded RFCOMM bootstrap. A handoff for a
  different identity on a bound activation fails with `rebind`, receives no
  credential bytes, and the helper rekeys the AP.
- The helper firewall drops the projection port unless it arrives on the AP
  interface, so a connection on that port came from a station that completed
  WPA2 with that activation's PSK.

The receiver admits an accepted TCP connection only while the coordinator is
admitting a trusted phone and its validated snapshot generation equals the
generation the listener was started from.

Limitations: the binding proves possession of the activation's PSK, not the
phone hardware. A device with which the authorized phone shares that network
can connect, consistent with the plan's phone identity boundary. Local root on
the receiver host is outside the threat model. A second approved phone that
hands off forces a rekey and ends the first phone's idle admission.

## Revocation and shared-AP disruption

Committed `ForgetPhone`, including committed-not-durable error 703, rotates AP
credentials and evicts **all AP access by design**. Shared-AP disruption is
expected. This does not retire unrelated trust, bonds, sources, or bindings;
an unrelated pending binding survives. The inherited 1000 ms total attempt,
500 ms per-leg cap, and 703-wins retry rules are unchanged.

## Host evidence and limits

```sh
cmake --build build/host-dev -j2
ctest --test-dir build/host-dev -R wireless_e2e --output-on-failure
ctest --test-dir build/host-dev -R wireless_recovery --output-on-failure
python3 -B tools/wireless-qa/wireless_recovery_qa.py run --build build/host-dev \
  --out .omo/evidence/jetson-android-auto-receiver/task-38-wireless-recovery.json
```

The 27 `wireless_e2e` groups run the D6 matrix. The `wireless_recovery` groups
run R01-R11 plus validator mutation gates; see
[qualification/wireless-recovery-qa.md](qualification/wireless-recovery-qa.md)
for the task38 evidence contract, blocked scenarios, and sign-off status. One end-to-end case composes the
actual `aa-system-helper`, private NetworkManager and BlueZ wire doubles, real
RFCOMM bytes, real `BusClient`, the trust/control path, TCP, and AAP TLS through
ACTIVE with media, audio, and input routing.

Coverage is not exhaustive. Secret scans, race/shutdown cross-products, and
full transcript comparison cover only part of their respective D6 subclaims.
Real-phone stimulation is **BLOCKED** and closes only as completed-reduced-scope
with owner sign-off. Real phones, RTL8822CE channel 36/DHCP/firewall behavior on
Jetson, installed-service and sandbox behavior, RSS/timing, and phone ordering
of `WifiConnectStatus` relative to AAP negotiation remain **TARGET BLOCKED**.

## Non-goals

No phone-hosted hotspot, Wi-Fi Direct/P2P, A2DP, AVRCP, SAP, HSP, or HFP
call-control/audio routing is added; the scoped HFP Hands-Free unit presence endpoint above
must not be read as telephony qualification. Performance and soak are task39, the wireless qualification package is task40,
and packaging is task41. The task38 host recovery matrix does not close those
scopes, real-phone scenarios, or full wireless
qualification.
