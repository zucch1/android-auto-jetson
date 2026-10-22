# Task36: exact-peer BlueZ pairing host reference

## Boundary and source evidence

The actual `aa-system-helper` executable instantiates `BluezBackend`. GIO is
private to `src/helper/bluez_adapter/`; the existing raw receiver/helper IPC,
action numbers 1-10, unique receiver name/UID/PID/starttime/executable digest,
source-owned five-second one-use grants and consume-before-effect checks remain.
The composed executable also instantiates NM and routes by actual operation.
Cleanup and ticks reach both; NM retains its dedicated worker. The explicit
standalone BlueZ fixture keeps AP unavailable, not implicitly completed.

OAA is pinned by `third_party/provenance/inventory.json` to commit
`61eab61c5f9968154ff1a80faa8c0a427b208479`, tree
`332f633e14dc23bbe4227eb3bde6242d0878fb00`. Its vendored README links the
[Bluetooth setup guide at that exact pin](https://github.com/mrmees/open-android-auto/blob/61eab61c5f9968154ff1a80faa8c0a427b208479/docs/wireless-bluetooth-setup.md)
(the guide is outside the vendored subset). It describes the AA service UUID
`4de17a00-52cb-11e6-bdf4-0800200c9a66`, RFCOMM server channel8 and the subsequent
Wi-Fi protobuf handshake. This is reverse-engineered research, **not an official
Google interoperability guarantee**. The backend uses only that AA UUID/server
role/channel, with authentication and authorization REQUIRED. It does not adopt
the guide's authorization bypass, HFP/HSP advice, legacy SDP/compat changes or
global plugin changes. No SPP, A2DP, AVRCP, SAP or HFP/HSP profile is registered.
Phone interoperability without the guide's audio-profile advice is BLOCKED.

API references: [BlueZ Profile API](https://bluez.readthedocs.io/en/latest/profile-api/),
[Agent API](https://bluez.readthedocs.io/en/latest/agent-api/), and GLib GIO
`g_dbus_connection_register_object`, thread-default main contexts,
`g_dbus_message_get_unix_fd_list` (borrowed list) and `g_unix_fd_list_get`
(owned close-on-exec duplicate). Agent capability is `NoInputNoOutput`;
`RequestDefaultAgent` must succeed to mediate unsolicited Just Works attempts.
PIN/passkey/numeric-comparison paths are rejected, not silently approved.

## Cross-process transaction

1. The registered consumer's authenticated `RequestAddPhone` allocates a one-time
   ID. `BusClient::prepare` polls helper-observed BlueZ objects. The host reference
   requires exactly one adapter and exactly one public-address device; no random
   selection, supplied paths, addresses in `channel`, or approval booleans.
   It uses already observed BlueZ Device1 objects and does **not** start discovery.
   No candidate remains a hard rejection. Discovery/stimulation on a real phone
   is a named deployment blocker, not an implicit scan window.
2. The helper returns a random opaque handle, address and adapter MAC. Internally
   the handle captures device/adapter object paths, BlueZ unique owner and a
   monotonic60s expiry. The receiver binds it to the request source, and the
   session/store owner calls `PairingPolicy::note_candidate` through the narrow
   `note_wireless_candidate` seam before `pairing_requested` snapshots identity.
3. Only the same registered consumer may confirm. `PairingPolicy` persists the
   exact snapshot. The bridge issues the existing separate AP/pairing grants and
   **executes the pairing grant**. The helper consumes it, revalidates process,
   source, handle, owner and actual identity, then opens Pairable/Discoverable
   for at most60s. Only an active result permits `PairingOpened`. A failed helper
   operation never implicitly erases already committed approval. AP's grant is
    retained for the separately requested AP acquire; pending AP status is not active.
4. Every effect-bearing Agent1/Profile1 callback authenticates the CURRENT BlueZ
   owner, exact device identity, and deadline; service authorization accepts only
   the AA UUID and an actual paired bond. Unknown Just Works is rejected outside
   the explicit window. Approved reconnect still requires a fresh observation,
   exact full store lookup on its owner thread and actual BlueZ Paired=true AND
   Bonded=true. Missing Bonded is rejected, never treated as an implicit bond.
   It enables only incoming AA service authorization, never re-pairing and never
   generic Device1.Connect. Targetless action9 is rejected.
5. NewConnection validates the real Unix-FD index, duplicates and owns one socket,
   rejects wrong/duplicate connections, and closes it on RequestDisconnection,
   cancel, idle expiry, owner change, receiver loss and teardown. No RFCOMM bytes
   are read/written: **task37 owns Wi-Fi credential exchange and session/media**.

Cancellation/failed trust confirmation discards the bound observation. Ordinary
Invalidate/teardown quiets the adapter and closes sockets but never RemoveDevice.
Before deletion, PairingPolicy/WiredPhoneControl captures the immutable full identity
on the store-owner thread. Only durable success or committed-not-durable703 sends
RevokeAP1 and, for wireless, RevokePeerV1. Precommit failures preserve trust and external bonds. The backend
accepts revocation only for an identity it managed, resolves the CURRENT object,
and calls Adapter1.RemoveDevice only for that exact peer. External failures emit
per-leg pending diagnostics and retain snapshot/progress in ControlService alone.
Authenticated ForgetPhone retries one phone without deleting again or resending
acknowledged legs; original703 wins every retry. The client owns no separate queue.
Pending exact wireless identities cannot be reapproved. No backend opens or
even receives the user's ApprovedPhoneStore/file.

## Host verification and deployment blockers

`tests/helper/bluez_runtime.py` starts two fresh private buses, a BlueZ wire mock,
the actual production helper, and a receiver fixture composed of the REAL
ControlBusService/ControlService/PairingPolicy/ApprovedPhoneStore/BusClient. Its
recording Backend is not used in the BlueZ scenarios. The raw peer probe is a
separate authenticated executable for handle/grant adversaries. Fault-injected
fsync affects only the receiver fixture; production trust persistence is unchanged.
The suite uses a real60s monotonic wait, not a configurable production deadline.

```sh
cmake --preset host-dev -DAA_GOOGLETEST_ARCHIVE=/home/zucchi/android-auto-jetson/.omo/worktrees/delivery-task27/.local/dependencies/googletest-v1.15.2.tar.gz
cmake --build build/host-dev --parallel 2
ctest --test-dir build/host-dev -R 'system_helper|trust_store|dbus_contract|architecture_boundary' --output-on-failure
python3 -B tests/helper/bluez_runtime.py build/host-dev
```

Composition evidence is under `.omo/evidence/composition/implementation/`. No task closure,
real-phone/hardware pairing, target custom-profile registration, installed service,
target latency/RSS, RF discovery effectiveness or Google interoperability is
claimed. No packages/services were installed and no host system bus was used.

Deployment remains BLOCKED on these explicit limitations:

- Real phone stimulation must supply an unambiguous already observed candidate
  while the helper remains non-discoverable/non-pairable by default. The host
  reference rejects multiple adapters/devices instead of inventing candidate UI.
- Real BlueZ/phone support for authenticated AA-only channel8 and default-agent
  permission and the Device1.Bonded property must be qualified; adding audio
  profiles or accepting a temporary non-bonded pairing is not an allowed fallback.
- The helper managed-peer set and receiver pending records are process-lifetime state.
  After helper/receiver restart, previously managed bonds cannot be blindly
  removed; an unknown managed identity is explicitly rejected. Crash-durable
  managed metadata/revocation reconciliation needs a separately reviewed fixed
  root-owned ledger, never access to the user store. Until then, operator recovery
  of a pending revocation across restart is required; no crash-safe claim.
- The reference systemd unit masks home directories (`ProtectHome=yes`), admits
  AF_UNIX plus NM command AF_NETLINK, and keeps strict system protection. No root user-store path
  is added. Actual proc/executable inspection, BlueZ/system-bus policy/default-agent
  permission and sandbox behavior must be verified on target before installation.
- Local composition integrates exact revocation with AP PSK rotation/client eviction.
  Task37 must provide RFCOMM Wi-Fi handoff/session
  ownership. This feature alone cannot establish wireless projection.
