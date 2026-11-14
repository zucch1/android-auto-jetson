# Enrolment ordering

## Bond states

Every bond the helper knows is in exactly one state. The helper decides cleanup,
admission and revocation from this state alone, never from BlueZ's global bond inventory.

| State | How it is entered | Persisted as | Services / reconnect | Cleanup |
|---|---|---|---|---|
| unaccepted | an enrolment window opens for the observed identity | entry in `pending-removals` (written before the window opens) | refused | removed when the window closes, or on helper start |
| provisional | the owner accepted the number, the helper verified the exact bond and the receiver acknowledged it (`PairingBondAcknowledgedV1`) | the same `pending-removals` entry, kept; the acknowledgement is only an in-memory flag of the current window | refused | removed when the window closes, on receiver loss, or on helper start |
| admitted | the receiver's durable trust write succeeded and the receiver committed (`PairingBondCommittedV1`) | entry in `managed-bonds` with a confirmed directory sync (#117), and **then** the `pending-removals` entry durably removed | allowed (Android Auto RFCOMM, HFP, `ReconnectPeerV1`) | none: the receiver owns it |
| revoked | `RevokePeerV1` (ForgetPhone, or the receiver's retraction of a failed enrolment) | entry moved from `managed-bonds` to `revoked-bonds` (history first, then removal, so authority never lapses) | refused | removed by the revocation itself; history is revocation authority only |

Admission is checked by one predicate: on `managed-bonds`, durably synced, and not on
`pending-removals`. Cleanup follows `pending-removals` alone. A journal entry leaves that
list only at a successful commit or after cleanup removed its bond, so every remaining
entry is a bond that was never admitted in its enrolment, and a `managed-bonds` or `revoked-bonds` entry for the same
adapter/address never suppresses its removal (finding B: a forgotten phone's history is
never taken as acceptance of a new bond). A managed entry left behind by a failed commit
is demoted before cleanup drops the journal entry, so a removed bond never becomes
admitted later. An identity that is currently on `managed-bonds` cannot open a new
enrolment window; it must be forgotten first, so a fresh enrolment of a known identity
always starts from scratch and its cleanup applies.

On helper start every `pending-removals` entry is removed (provisional ones included),
because no commit for it can arrive any more: the acknowledgement lives only in memory.
Reconciling against the receiver instead was rejected. The helper has no channel to ask
the receiver (all calls are receiver-initiated and authenticated as such), the receiver
may itself be restarting, and removal is the fail-closed outcome: if the receiver had in
fact written its approval, its commit fails and it retracts that approval (below).

## Order

`ConfirmPhonePairing` opens helper admission. For the production wireless helper,
this is an in-memory pending enrolment, not approval. The consumer must retrieve
the bound six-digit challenge and explicitly accept its one-time token. The
receiver then polls `PairingBondConfirmedV1`: a verification only, which checks that
the exact captured BlueZ owner, device and adapter incarnations still exist and are
bonded after the consumed acceptance, and records nothing. Once it reports a bond,
the receiver runs every receiver-side check (deadline through the single
`PairingPolicy::expired()` predicate, consumer ownership, store reload/snapshot) and
only then calls `PairingBondAcknowledgedV1`, which makes the bond provisional and
writes nothing. After the acknowledgement the receiver reconciles deadline, owner and
snapshot once more (the RPC can dispatch changes) and then calls the shared
`persist_pairing` routine, which calls `PhoneDirectory::pairing_confirmed`. Only when
that write is durable does the receiver send the second, explicit step,
`PairingBondCommittedV1` (same sender, UID and lease checks as every comparison
method). The helper re-verifies the exact bond, writes `managed-bonds` with a confirmed
directory sync and then durably removes the journal entry; only now are services and
reconnect allowed. The non-comparison flow continues to use the same persistence
routine after helper confirmation (M10).

A failure before the acknowledgement closes the enrolment and the helper removes
the unaccepted bond. A failure after it and before the commit (a failed or lost
acknowledgement, a deadline, owner or reload change in the acknowledgement window,
or the trust-store write itself) leaves a provisional bond with no store entry. The
receiver retires it through the existing tracked revocation record (`revocations_`,
as `ForgetPhone` uses it): bond leg pending, AP leg complete (nothing was ever
approved), plus the helper source retirement, retried by #113's single bounded retry
loop; its captured identity blocks re-enrolment until the bond is removed. Its cleanup
does not depend on that record: the journal entry is still in place. If the receiver
process exits at this boundary (finding A), its in-memory record is lost, but the
helper's identity-loss cleanup, or its next start, removes the bond from the journal.

A failed or lost commit, or a write that landed only as a committed-but-not-durable
store result (703), is not admitted: the helper closes the window (no services) and
leaves the bond to cleanup, and the receiver retracts its approval through the same
path as `ForgetPhone` (store forget plus tracked `RevokePeerV1`), emitting
`pairing_commit_failed`. A receiver crash after its store write and before the commit
leaves an approval whose bond the helper removes and never admits; that phone cannot
reconnect and has to be forgotten and enrolled again.

`RevokePeerV1` accepts an admitted, revoked or pending-removal identity. It also
reports success, touching nothing, for an identity this helper run's cleanup already
removed while no device with it is present, so a receiver retry after the cleanup won
the race can complete; that in-memory record is no authority and is lost on a helper
restart. Every other identity is refused.

No deadline or background callback supplies acceptance.

Services (`AuthorizeService` for HFP and the Android Auto RFCOMM channel, and the
profile's `NewConnection`) and `ReconnectPeerV1` require the admitted state, so they
are refused while the bond is unaccepted or provisional and are allowed only after the
commit and its confirmed `managed-bonds` sync (finding C). Whenever an enrolment fails
to complete, the helper disconnects the device (BlueZ `Device1.Disconnect`) before it
removes the bond, on both retirement paths (journal cleanup and `RevokePeerV1`), so a
removal that keeps failing cannot leave an unadmitted phone's link connected. The
disconnect is best effort and is retried with each removal attempt.

During the whole comparison flow the helper's BlueZ agent accepts exactly one
pairing method: the `RequestConfirmation` whose number the owner was shown.
`RequestAuthorization` (Just Works, no number), `RequestPinCode`,
`RequestPasskey` and every other agent method are always refused. The owner's
acceptance is consumed once, by the reply to that `RequestConfirmation`; pairing
(`Pairable`, `Discoverable`) closes immediately after that reply. If the genuine
pairing then fails, no other device (for example one spoofing the phone's
address) can pair into the window, and a bond that forms without the consumed
acceptance is never recorded as managed. The acceptance flag is cleared whenever
admission closes.

Rejection, cancellation, expiry, consumer loss, receiver shutdown, helper failure
and store reload discard pending identity snapshots, close the gate and emit a
reason diagnostic. Reload attempts advance the store epoch, including failed
loads. Helper lease retirement removes unadmitted devices through
BlueZ RemoveDevice; failed removal stays queued for retry on helper ticks, even
if BlueZ temporarily disappears. The queue is persisted next to `managed-bonds`
as `pending-removals` (same root-owned directory, owner-only regular file,
no-follow traversal, exclusive staging, fsync and atomic rename, at most 256
records) and an entry is written before any window opens, so a helper restart
completes a removal that was pending retry; an admission whose entry cannot be
written is refused. Rebuilding the queue from BlueZ was rejected: BlueZ cannot
distinguish a bond an enrolment window created from one the owner made, and
removing the latter would be destructive. The list is not revocation authority
on its own once its entry is gone. An entry leaves it at a successful commit or
once its device is gone from BlueZ; a removal that succeeded but whose record
cannot be rewritten is remembered in memory so a device later paired again at
that address is not removed on every tick. `revoked-bonds` uses the same file
rules and, when full, evicts its oldest entry. Admitted bonds retain main's
CancelPairing guards. An abrupt receiver exit triggers helper identity-loss
cleanup. No production state or installed helper is exercised by host tests.

The helper RPC is a versioned extension: an older installed helper fails closed.
The host suite does not qualify phone pairing, controller key persistence, HFP
interoperability, AP association or live wireless projection. Issue #72 remains
open: its persisted-key/controller reconnect failure needs separate live evidence.
