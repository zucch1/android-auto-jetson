# Composed privileged system helper (tasks 34/35/36, host reference)

## Scope and trust boundary

`aa-system-helper` is a C++20 daemon using the existing POSIX control D-Bus
stack and OpenSSL Crypto. The actual executable constructs both the NM AP worker
and private GIO/gio-unix BlueZ adapter. An explicit operation router sends AP
acquire/release/status/revocation to NM and peer/profile/pairing operations to
BlueZ, without try/fallback dispatch. Cleanup and platform ticks reach both.
It handles no media, Wi-Fi credential exchange, arbitrary receiver-supplied commands
or network profiles. Bluetooth identity data is used only by the narrow
versioned observed-peer API below. USB policy and ModemManager exclusion remain
status seams, not policy writers. See [task36-bluez.md](task36-bluez.md) for
the exact source evidence, authorization contract and target blockers.
See [networkmanager-ap.md](networkmanager-ap.md) for the configured UNII-1 AP channel, explicitly
confirmed jurisdiction, root-only credentials and isolation/shutdown policy.

The receiver executable is the trust anchor, **not cryptographic attestation of
human clicks**. An attacker able to run/modify the trusted receiver's code or
inject into its process can invoke its authority. Same-user debuggability,
package integrity, process hardening and active-session provisioning remain
deployment obligations. The helper does not infer the session UID from its
first caller, a signal, an end-client boolean, or a well-known receiver name.

Each call resolves the daemon-stamped **unique** system-bus sender on a separate
credential connection with `GetConnectionUnixUser` and
`GetConnectionUnixProcessID`. It verifies all four proc status UIDs, anchored
proc-directory ownership, live process state/starttime, and SHA256 of an opened
`/proc/PID/exe` FD. That leaf is deliberately followed (it is a kernel symlink).
The executable inode and process state/starttime are rechecked after hashing.
The pinned tuple is unique name, UID, PID, starttime and digest. Executable
content is rehashed on subsequent calls: `exec` does not change starttime.
Effects get a second identity validation immediately before backend dispatch.
This narrows races; it is not a kernel atomic exec-and-effect transaction.

## Explicit fail-closed provisioning (not installed by this task)

Package/operator provisioning must create these root-owned regular files:

* `/etc/android-auto-receiver/session.uid`: decimal numeric expected session UID
  and optional single LF. No sign, whitespace, comments or trailing fields.
* `/etc/android-auto-receiver/receiver.sha256`: exactly 64 lowercase hex SHA256
  digits and optional single LF, calculated from the installed receiver binary.
  This is **not** `sha256sum`'s digest-plus-filename format.

Every path component must be root-owned, free of group/other write permission,
and not a symlink; final files must be regular and at most 128 bytes. Suggested
directory mode 0755, file mode 0600. Missing/invalid provisioning aborts startup.
An executable update requires an intentional digest update and receiver/helper
restart; live session-user changes likewise require explicit reprovisioning.
There is no first-caller enrollment, ambient environment bus override, automatic
configuration generation or installation/activation in this implementation.

`packaging/systemd/aa-system-helper.service` is a reference root service;
`packaging/dbus/org.custom.AndroidAutoSystemHelper.conf` reserves bus ownership
for root and denies helper method delivery by default. Installation must add a
root-owned, non-writable policy fragment granting delivery only to the explicitly
provisioned receiver account, whose numeric UID must match `session.uid`. For
example, substitute that actual numeric UID for `RECEIVER_UID` before installing
this fragment; never install the unresolved placeholder or a default-context
allow rule:

```xml
<busconfig>
  <policy user="RECEIVER_UID">
    <allow send_destination="org.custom.AndroidAutoSystemHelper"
           send_interface="org.custom.AndroidAutoSystemHelper1"
           send_type="method_call"/>
  </policy>
</busconfig>
```

Without this explicit per-user provisioning the receiver is denied access.
Delivery permission is **not** authorization: every helper method still
authenticates the receiver. Review sandbox/ptrace/proc access against the deployed
receiver and system-bus policy before installation. No systemd D-Bus activation file is
provided. No unit/policy was installed or activated during host verification.

Production takes no arguments and always connects to
`unix:path=/run/dbus/system_bus_socket`. For explicit **non-root, non-setuid**
host verification only, the same binary accepts
`--private-bus ADDRESS POLICY_DIRECTORY`; it uses current-UID policy ownership,
still protects the immediate policy directory and files, and allows
current-UID-owned group-writable checkout ancestors (never world-writable).
This four-argument mode cannot be selected by root and uses the real BlueZ adapter
with AP explicitly unavailable. Adding a command-socket argument selects standalone
NM with BlueZ unavailable. `--private-bus ADDRESS POLICY_DIRECTORY --combined SOCKET`
selects both real adapters with the non-executing private command boundary.
Production always requires valid `ap.conf` and constructs both adapters; no ambient
environment switch changes capabilities or the privileged command boundary.
The recording backend exists only in the separate test fixture executable.

## Receiver integration and partial effects

Inject `helper::BusClient` through the optional `ControlBusService::Deps`
`confirmation` pointer (and use that same instance as `HelperClient`). One internal
transaction lock serializes all confirmation, cancellation, status, reconnect and
effect calls, including grant-state changes and the entire socket RPC/reply.
Both Issue calls for a confirmation hold the same lock. Private failure cleanup
uses the existing lock rather than acquiring it recursively.

This lock does not make `ApprovedPhoneStore` thread-safe. All trust-store reads
and mutations, including `reconnect()` lookup and control/trust dispatch, must
remain on the store-owning session thread as required by its existing contract.
The helper-only operations can share the client from another thread; destruction
requires callers to have finished. Wired clients without this dependency do not
gain helper authority: committed forget now reports required AP work as pending,
not implicit cleanup success. The actual `ControlService::confirm_phone_pairing`
path invokes the helper after registered-consumer UID/name authorization,
`PairingGate::confirm`, and policy validation of the live bound request and
deadline (`PairingPolicy::expired()`, the one predicate), before any phone
approval is persisted. Policy rechecks the same predicate after the helper RPC;
expiry retires helper authority through the tracked cancellation path and
persists nothing. For numeric comparison this only opens a pending in-memory
enrolment. The consumer must retrieve and explicitly accept the one-time
comparison; the comparison-confirm path applies the same pre- and post-helper
liveness checks before and after activating the helper. The helper must verify
the exact bond through `PairingBondConfirmedV1` (which records nothing); the
receiver then runs its own checks and marks it provisional with `PairingBondAcknowledgedV1`
(no write), the shared approval routine writes the trust store, and only a durable write is
committed with `PairingBondCommittedV1`, which admits the bond; a failed commit retracts the
approval (docs/enrolment-ordering.md). `PairingOpened`
describes admission and follows committed approval, including error 703 (rename
committed, directory durability uncertain), which emits
`pairing_approval_not_durable` and keeps helper confirmation consistent with the
store. Listening for or forging that signal never mints grants. No session D-Bus
method proxies Issue. A helper error never persists approval.

If persistence fails before commit or any pairing cancellation/expiry fails
helper invalidation, cancellation must be acknowledged. A failed cancellation
emits `phone_revocation_retirement_pending` and retains the source and captured
identity in the existing revocation progress queue, blocking new pairing for
that identity until acknowledged `Invalidate` retirement. A bond the helper may
already manage (acknowledged, then a failed store write) uses the same queue with a
bond leg. All of these phone-less records retry through one bounded loop: a
per-invocation budget (4 attempts or 500 ms), a round-robin cursor and exponential
backoff of 1-30 s measured from the end of each attempt, during owner
reconciliation (including idle calls); ordinary phone revocations retain their
existing ForgetPhone retry path. Cancel, consumer teardown/owner loss,
successful or committed-not-durable forget, and control-service shutdown
invalidate helper authority. Committed forget invalidates
only the retired phone's sources locally and remotely, then sends narrow revocation
requests. `RevokeAP1` rotates/evicts AP access without clearing unrelated grants or
bound observations: it has no phone-to-source map. Failed remote invalidation also
discards matching local grants; the helper independently expires grants and detects receiver loss.
Control-service owner refresh runs at least once per second even under traffic.
Typed effect requests may carry explicit consumer-generation/confirmation IDs;
the zero/default selects only the bridge's current active
source. Grants are selected by source and action together, never action alone.
A source-scoped lease cannot be replaced by another confirmation's effect
until cleaned; cancelling an unrelated source does not clean the owning lease.

`BusClient::reconnect(TransportIdentity)` performs the actual receiver-side
`ApprovedPhoneStore::lookup` of the validated **full exact identity** before
calling the helper. Unknown, mismatched, unusable-store and forgotten identities
fail closed. There is no public approved boolean. The helper returns an opaque,
expiring peer observation plus exact address/adapter identity. The receiver
compares that observation to the full approved identity and sends only its handle
to `ReconnectPeerV1`; the helper independently requires Paired AND Bonded from
the current BlueZ Device1 (temporary pairing is not a persisted bond).
The helper trusts the store decision only because of the receiver
executable/process binding, not because it independently has the user store.

### Receiver-owned revocation V1

`PairingPolicy` and `WiredPhoneControl` capture an immutable
`RevocationV1{PhoneId, TransportIdentity}` on the store owner before deletion;
`StorePhones` forwards the owner-queue snapshot. Only successful deletion or
committed-not-durable703 permits external effects. Missing committed snapshots
are contract errors, never ordinary-cleanup success. Precommit failure preserves
trust and external state.

`ControlService` alone owns up to256 pending records: snapshot, AP completion,
optional bond completion and original durability result. AP revocation is required
for wired, wireless and migrated opaque identities. Wireless additionally requires
the captured exact address+adapter bond. `forgotten_v1(snapshot, progress)` reports
both independent results; AP is attempted first, bond even if AP fails. Acknowledged
success is monotonic and never resent. The client has no revocation retry queue.
Each attempt shares at most1000ms starting before timed mutex acquisition, including
associated local authority cleanup; each leg has at most500ms, with time reserved
for the second. Unavailable capabilities remain pending.

Repeated authenticated `ForgetPhone` for a pending ID retries that one record,
without store deletion or duplicate PairingClosed. New deletion is rejected before
mutation if all256 slots are occupied. Matching open/pending pairing IDs close
once after commit; unrelated requests/history are preserved. The captured wireless
identity cannot be reapproved until its pending record completes. Consumer teardown
and helper-owner change do not erase committed progress. Fixtures observe count
and per-phone progress from ControlService.

Original703 wins every external result on the initial attempt and every retry.
For durable deletion an incomplete AP error precedes an incomplete bond error;
fully completed external work returns success. Diagnostics reference PhoneId:
`phone_revocation_ap_pending`, `phone_revocation_bond_pending`, and
`phone_revocation_external_complete`. Only fully completed records are erased;
later missing-ID calls use normal store semantics, not indefinite success history.
Lost replies remain unknown outcomes: this is neither globally exactly-once nor
crash-durable revocation. NM retains its in-flight rotation latch. BlueZ managed
identities are durably recorded in `/var/lib/aa-system-helper/managed-bonds`
(versioned exact address/adapter pairs, at most 256). The helper commits the
allow-list only when the receiver calls `PairingBondCommittedV1`, after explicit
comparison acceptance, exact bond verification, every receiver-side check and the
receiver's durable trust write. Pending admission and acknowledgement never write `managed-bonds`;
admission records the identity in `pending-removals` (same file rules, not revocation authority)
so a restarted helper still removes an unaccepted or provisional bond. Revocation moves an
entry from `managed-bonds` to `revoked-bonds` (same file rules, at most 256, oldest evicted):
revocation authority only, never admission.
A trusted reconnect is admitted only for an
identity already on the list whose directory entry is durable (loaded after a confirmed
directory fsync at startup, or committed by a successful one; a pending sync is retried on
reconnect) and never adds one. Revocation depends only on presence, never on durability. It loads
only an owner-only regular file in a private, owner-validated directory, using
no-follow path traversal. Writes use an exclusive, randomly named 0600 staging file (EINTR retried), file
fsync, atomic rename and directory fsync. A write failure refuses the new bond acknowledgement; revocation of every entry already stored, or already renamed into
place when only the directory fsync failed, keeps working. After a failed
directory fsync a new bond acknowledgement for that entry requires a later successful fsync.
Stored state that is invalid at startup makes the helper log the reason and
exit, so bonds are never forgotten silently; global BlueZ inventory never grants
revocation authority. Revoked entries are retained in `revoked-bonds` for idempotent revocation
retries; `managed-bonds` holds only currently admitted bonds, and an identity on it
cannot open a new enrolment until it is revoked. Bonds managed before this state format was deployed
cannot be inferred safely and need explicit operator removal; they must be
re-paired before they can reconnect. A staging file left by a power cut has a
random name, blocks nothing and may be deleted by an operator.

## System bus API 1.4.0

Name `org.custom.AndroidAutoSystemHelper`, path
`/org/custom/AndroidAutoSystemHelper`, interface
`org.custom.AndroidAutoSystemHelper1`. XML is in `docs/dbus/` (reference schema;
the helper does not expose Introspectable). Methods:

| Member | In | Out | Meaning |
|---|---|---|---|
| Issue | `ttuu` generation, confirmed source, action, channel | `ut` error, grant | Trusted receiver bridge only |
| Execute | `uut` action, channel, grant | `ut` error, state | Strict typed action dispatch |
| Invalidate | `tt` generation, source | `ut` error, reserved zero | Zero selects all generations/sources |
| RevokeAP1 | `t` committed forgotten phone ID | `ut` error, reserved zero | Shared AP rotation/eviction, same authenticated receiver binding |
| PollPeerV1 | empty | `utss` error, handle, address, adapter identity | Exactly one helper-observed public BR/EDR candidate, otherwise reject |
| BindPeerV1 | `ttt` generation, request source, observed handle | `ut` error, zero | Snapshot binding before confirmation, no radio exposure |
| ReconnectPeerV1 | `t` observed handle | `ut` error, state | Store-owner exact lookup followed by helper actual-bond check |
| RevokePeerV1 | `ss` immutable removed address, adapter identity | `ut` error, zero | Only an admitted, revoked or pending-removal exact identity (or, reporting success with nothing to do, one this helper run's cleanup already removed and no longer present); demotes an admitted bond to `revoked-bonds`; re-resolve BlueZ object, never accept a supplied path |
| PairingBondConfirmedV1 | `tt` generation, request source | `ut` error, bonded boolean | Verification only: accepted comparison and exact bonded snapshot; records nothing |
| PairingBondAcknowledgedV1 | `tt` generation, request source | `ut` error, reserved zero | Marks the verified bond provisional after every receiver-side check passed; writes nothing, admits nothing |
| PairingBondCommittedV1 | `tt` generation, request source | `ut` error, reserved zero | After the receiver's durable trust write: admits the provisional bond (confirmed `managed-bonds` sync, then durable `pending-removals` removal); any failure closes the window and leaves the bond to cleanup |
| GetPairingComparisonV1 | `tt` generation, request source | `utuss` error, challenge ID, comparison, address, adapter | Authenticated consumer-only retrieval during explicit enrollment |
| RespondPairingComparisonV1 | `tttu` generation, request source, challenge ID, accept | `ut` error, reserved zero | Explicit human decision after retrieval; one use, same enrollment/peer/deadline |

Errors: 0 success, 1000 unavailable, 1001 rejected, 1002 protocol mismatch.
Nonzero errors carry value zero. States: 0 inactive, 1 active, 2 unavailable.
Unknown interfaces/paths/members/signatures fail closed. Non-method traffic is
dropped. Method replies are matched by serial, daemon-stamped helper unique
sender and destination; name handover cannot redirect an in-flight call.

Actions (wire numbers are append-only):

| Number | Action | Authorization argument |
|---|---|---|
| 1 | AP acquire | channel 36/40/44/48 and equal to ap.conf (default 36); one-use AP grant |
| 2 | Open pairing (fixed backend 60-second-window contract) | channel 0; distinct one-use pairing grant and captured peer binding |
| 3 | AP release | channel 0, grant 0, same receiver binding |
| 4 | AP status | channel 0, grant 0, same binding |
| 5 | USB policy status | channel 0, grant 0, same binding |
| 6 | ModemManager exclusion status | channel 0, grant 0, same binding |
| 7/8 | BlueZ register/unregister fixed AA profile | channel 0, grant 0, same binding |
| 9 | Reserved targetless reconnect | Rejected; use ReconnectPeerV1, no renumbering |
| 10 | Close pairing | channel 0, grant 0, same binding |

Issue grants are random server-side IDs distinct from confirmation IDs and
bound to the pinned receiver, consumer generation, confirmed source, action,
channel and steady-clock expiry (5 seconds). AP and pairing cannot share a
grant. Duplicate source/action issuance fails even after consume, cancel,
expiry or loss. Pending grants are capped at 16; lifetime replay history at
4096. Exhaustion fails closed, never evicts history, and requires operator
restart. All grants are consumed before revalidation/backend effects; backend
errors and ambiguous timeouts do not permit retries with the same grant.
An AP acquire returning inactive/pending is **not applied**. Subsequent ensure
calls for the same source poll AP status without renewing or resending a consumed
grant; only verified active state maps to `Response.applied=true`.

## Loss handling and bounds

One receiver owns helper authority at a time. A steady-clock sweep scheduled
every second runs regardless of incoming traffic. Receiver unique-name loss,
death, UID/starttime/digest change and system-bus failure invalidate pending
grants and clean the stored owning backend lease internally (no attempt to
authenticate a dead process). Graceful helper destruction also cleans leases.
The GIO adapter services callbacks and monotonic deadlines on a helper-local
context every cycle (poll capped at 20ms); receiver identity checks remain on
the independent one-second cadence. BlueZ calls have a 150ms deadline, owner
lookup 100ms, no auto-start and a captured unique destination. BlueZ daemon
owner changes invalidate observations and close FDs. Adapter BlueZ timeouts are
also set to 60s. Cleanup retries failed quiet-property writes. Crash/restart
reconciliation limits are explicit in the task36 document, not target-qualified.

Credential RPCs share an absolute 500 ms deadline and helper export read/reply
steps 500 ms. Typed client calls use the caller's timeout capped at one second;
confirmation's two Issue RPCs plus pairing Execute, status, preparation and
reconnect each use a one-second budget.
These budgets begin before transaction-lock acquisition and include waiting.
Confirmation failure cleanup uses only the original budget's remaining time.
Void invalidation first waits for transaction ownership so local authority is
always erased, then allows 500 ms for its remote attempt; that is not a total
500 ms bound including lock waiting. The raw transport does not reset a timeout
for each byte or skipped message.

A call cancelled while waiting for ownership returns without consuming a grant
or touching the socket. Once it owns the transaction, cancellation shuts down
the client's dedicated connection; recreate it to resume. The cancellation
callback is destroyed before transaction ownership is released, so it cannot
interrupt a later caller. A cancelled/failed RPC may already have consumed a
grant or applied an effect; cancellation is not transactional rollback.

## Host verification and deferred qualification

From this worktree, without installing dependencies or touching hardware:

```sh
cmake --build build/host-dev --target aa-system-helper aa_system_helper_test aa_system_helper_fixture aa_system_helper_bus_test -j2
ctest --test-dir build/host-dev -R system_helper --output-on-failure
```

Tests use two fresh private `dbus-daemon` instances, actual receiver/helper
processes, real consumer request/confirm/trust persistence, wrong-executable
and unauthorized-consumer/forged-signal rejection, replay/expiry/budget and
identity adversaries, recorded receiver-crash cleanup under directed traffic,
bus-loss cleanup, and `/proc/PID/status` RSS measurement of the actual production
daemon under bounded status activity (ceiling 32 MiB). Writable fixtures are
under this worktree's build directory. Unit clock/identity seams cover process
reuse/starttime changes that cannot be reliably forced on a host kernel.
The directed-traffic fixture sends continuously at up to one message per
millisecond for four seconds. Cleanup's 10-second absolute deadline starts
before killing the receiver or terminating the private bus, including process
wait time, not after the disconnect has already completed.

The shared-client contention cases hold a recording backend RPC open using an
entered/release handshake, then exercise queued cancellation, queued timeout,
and real control confirmation/cancellation on the same client. Queued calls must
leave the active RPC, socket health and unused grant intact. Duplicate-confirm
failure also exercises cleanup without recursive locking. The delivery-policy
test loads the packaged policy into private buses: default access is denied;
an explicit fixture-UID delivery grant still requires executable authentication.

The task36 runtime suite additionally exercises the actual daemon and real
GIO adapter against a private-bus BlueZ mock, including Unix-FD transfer,
exact-peer callbacks, revocation703 and a real 60s idle window. Real AP,
USB-policy verification, ModemManager exclusion, target/phone BlueZ behavior,
installed sandbox and Jetson RSS/timing qualification remain **BLOCKED**.
Host fixture success is not wireless or target acceptance.

`system_helper_composed_runtime` drives the actual compiled daemon with BOTH NM
and BlueZ wire doubles, the fake privileged command socket, and a separate
receiver/control session bus. It covers confirmed pairing+AP, both asymmetric
revocation failures, credential stability on repeated bond retries, repeated703
failure precedence, precommit preservation, wired AP-only deletion, persistent
unregister and cleanup of both backends after receiver loss. This is a local
buildable handoff, not hardware qualification or final parent acceptance.

The synthetic cross-smoke sysroot does not contain OpenSSL development inputs.
Like the TLS qualification targets, the helper is explicitly unavailable in
that configuration; native host helper tests remain mandatory. The cross-smoke
configuration regression verifies that no target OpenSSL lookup is attempted,
not that the helper has been cross-built or qualified on ARM64.

Wireless trust-store upgrade recovery quarantines formerly accepted printable
non-MAC bond addresses and all records in groups differing only by bond-address
case. None of those records remains trusted or is merged. Before rewriting the
remaining approvals through the durable store writer, load saves the original
bytes next to the store as `<store>.legacy-backup`, owner-only, through the
verified descriptor path walk and exclusive creation. An existing private,
regular backup is retained unchanged; a symlink or unsafe backup aborts recovery.
A diagnostic reports the quarantined counts and reasons without identifiers.
Quarantined phones require explicit pairing again. Other corruption still fails
closed for the whole store; stop the receiver and preserve the original before
manual recovery. The backup is for inspection, not automatic trust restoration.
