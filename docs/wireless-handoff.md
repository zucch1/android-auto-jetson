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

The receiver plays the head-unit role. It opens with a version request, then
follows whichever path the phone's reply selects:

```text
receiver  WifiVersionRequest=4 {1.0, 5GHZ_ONLY}     authorized, listener ready

modern path:
phone     WifiVersionResponse=5 {1.0, status=12}
receiver  WifiSetupInfo=11 {endpoint {ip, port},
                            access_point {ssid, bssid, password,
                                          security_mode}}
receiver  WifiStartRequest=1 {ip, port, DYNAMIC}
phone     WifiStartResponse=7 {status field 4 = 12}
phone     WifiConnectStatus=6 {status field 2 = 12}

legacy fallback:
phone     WifiInfoRequest=2
receiver  WifiInfoResponse=3 {ssid, password, bssid,
                              security_mode, access_point_type}
receiver  WifiStartRequest=1 {ip, port}
phone     WifiStartResponse=7 {status field 3 = 0}
phone     WifiConnectStatus=6 {status field 1 = 0}
```

The version request is sent only after authorization and only once the
projection listener is ready. Credentials leave the helper only in the
modern `11` or legacy `3` record, from the pinned AP snapshot and behind the
same snapshot fence and eligibility checks described below. Both paths then
send the endpoint in message `1` and validate the trailing phone `7` and `6`.
The success value differs by path: the modern layouts use status `12` in
different field tags, while the legacy layouts use `STATUS_SUCCESS=0`. Any
other version, status, or path mixing is a phone rejection or wrong-order
failure.
Completion requires the trailing-data check, including a nonblocking socket
peek, and a deadline recheck; receiving one expected record is not enough.

On this AAW surface, `security_mode` uses its own bitmask-style enum with
`WPA2_PERSONAL=8`; `access_point_type` is `DYNAMIC=1`. The BSSID is the current
AP interface MAC as a lowercase, colon-separated string. The project-owned
`Bootstrap.proto` is intentional: vendored AASDK imports the negotiated Wi-Fi
projection enum, which is not the AAW bootstrap enum and cannot encode this
contract correctly.

Denial, malformed or oversized input, wrong order, duplicates, unknown IDs,
EOF, timeout, and phone-side rejection close RFCOMM without a rejection payload
or further credentials. The helper reports a typed local failure without secret
material and does not mutate trust. Normal closure after confirmed success has
the separate completed-handoff semantics described under lifetimes.

The wire rules are reverse-engineered, not an official Google interoperability
specification. Pinned head-unit-role references are
`nisargjhaveri/WirelessAndroidAutoDongle` at
`ef645221ec28e87361391b9bdcf27e31ff48cb6f` and `manio/aa-proxy-rs` at
`b753a002b7ff19f94a861a81396eacaccfa58d5e` for the legacy path, and
`mrmees/open-android-auto` at `61eab61c5f9968154ff1a80faa8c0a427b208479` for
the modern path. Both paths are exercised only by host tests against scripted
peers; that is not proof of interoperability with a real phone.

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

An incomplete bootstrap has a 10-second budget. The 60-second pairing window
limits only acceptance of new RFCOMM connections; it is not a TTL for an
established bootstrap, completed handoff, or projection session.

Successful completion transfers the exact handle, receiver/source ownership,
BlueZ owner/device identity, and pinned AP snapshot into revocable
completed-handoff authority. It survives normal RFCOMM close and the subsequent
Profile1 `RequestDisconnection` callback, and has no absolute TTL. Before
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
