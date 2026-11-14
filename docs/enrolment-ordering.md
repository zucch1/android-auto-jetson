# Enrolment ordering

## Authority rule

Two processes keep durable bond state: the receiver's trust store
(`approved-phones.json`) and the helper's `managed-bonds`, `pending-removals` and
`revoked-bonds`. They cannot be committed atomically together, so exactly one of them
is the authority: **the receiver's trust store decides which wireless bonds exist and are
admitted. The helper's files are a cache of that decision**, reconciled to it and never
trusted beyond it. Every crash window between the two writes is healed the same way: the
helper parks what it cannot decide, fail-closed, until the receiver answers.

## Reconciliation exchange

`ReconcileBondsV1(a(ss) approved) -> (u error, t info)` carries the set of wireless
identities the store approves (at most 256 exact `address adapter` pairs, no duplicates;
the body is bounded before parsing, as for every method since #95). It is authenticated
like every other helper method (sender, UID and receiver process identity, #104). It takes
no lease: it runs at receiver start, before any pairing source exists, like `RevokePeerV1`,
and it never touches the bond of the live lease's open enrolment window (that window
decides its own bond). `info` carries, in its low 32 bits, the approved identities left
unadmitted, and in bit 32 that an open window was skipped.

The receiver calls it whenever it (re)gains a live helper connection: on the first control
refresh after start, then once per second as a cheap check that sends the set again only
when the helper's unique name or the approved set changed (a reappeared helper is a new
incarnation), and immediately after anything that may leave the helper behind: an
enrolment that ends without a commit, a failed commit, a ForgetPhone, any failed
reconciliation. `BusClient::confirmed()` and `BusClient::reconnect()` also reconcile an
unreconciled helper incarnation first. If the store is unusable there is no authority to
send and nothing is reconciled, so the helper stays fail-closed.

The helper makes its state match, durably and idempotently:

| Helper state for the identity | In the approved set | Not in the approved set |
|---|---|---|
| on `revoked-bonds` (no enrolment since its revocation) | stays revoked: demotion completed, **not** admitted, bond not removed (#74); counted as unadmitted | history only, nothing to do |
| on `managed-bonds` | admitted; a journal entry left beside it (commit stopped between its writes) is durably removed | demoted: `revoked-bonds`, then out of `managed-bonds`, then the removal owed in `pending-removals`; device disconnected and removed |
| on `pending-removals` with a bonded device | admitted: `managed-bonds` synced, then the journal entry removed (the approval landed, the commit did not) | device disconnected and removed, then the entry dropped |
| on `pending-removals`, no bonded device | counted as unadmitted; cleanup drops the stale entry once the device is gone | entry dropped once the device is gone |
| nothing | counted as unadmitted (approved, but no bond to admit) | an unmanaged bond is never touched |

Revoked versus approved: only a new enrolment clears an identity from `revoked-bonds`
(`authorize`, durably, before its window opens; the journal entry holds revocation
authority meanwhile). So an approved identity still on `revoked-bonds` has no commit after
its revocation: it is a resurrected (non-durable forget lost to power loss) or stale
approval, and the revocation wins. A committed approval always wins, because its commit
went through a fresh enrolment that cleared the history. The owner can ForgetPhone such a
phone (it then completes) and enrol it again.

Until the first successful reconciliation of a helper run nothing is admitted for services
or reconnect, no enrolment window opens, and cleanup removes nothing, because any journal
entry may be an approval whose commit never landed; the receiver's answer decides. Every
helper state change confirms persistence (rename plus directory fsync) before it reports
success; on any failure reconciliation reports failure, the helper keeps the stricter state
(admission stays off for this run; an unwanted bond is still disconnected and removed), and
the receiver retries on its next pacing step.

## Crash windows

| Window | Durable state left behind | How reconciliation heals it |
|---|---|---|
| Receiver exits after the acknowledgement, before its trust write | journal entry, bond; no approval | the helper parks the bond (not admitted, not removed); the restarted receiver's set lacks it: removed |
| Receiver's trust write is durable, it exits before the commit (window 1) | approval, journal entry, bond | parked; the restarted receiver's set has it: admitted, approval kept |
| Commit sent, reply lost, or the helper's commit write failed | approval, journal entry (maybe also `managed-bonds`), bond | the receiver keeps its durable approval, retires the enrolment source (which parks the bond) and reconciles: admitted |
| Helper wrote `managed-bonds`, crashed before removing the journal entry (window 2) | approval, managed and journal entries | the restarted helper admits nothing; the live receiver sees the new incarnation and reconciles: journal entry dropped, admitted, bond kept |
| Trust write fails after the acknowledgement | journal entry, bond; no approval | the enrolment's source is retired and reconciliation removes the bond; no receiver record needed |
| Approval only committed-but-not-durable (703) | approval (maybe lost), journal entry, bond | the receiver retracts (store forget, tracked revocation) and reconciles; if the retraction cannot be written either, the approval remains and reconciliation keeps the helper consistent with it; after a power loss the next start's reconciliation decides from what survived |
| ForgetPhone durable while the helper is down, then the receiver restarts (finding 2) | no approval; helper still on `managed-bonds` | the receiver's record is lost but not needed: its start-up reconciliation demotes and removes the bond; the phone can enrol again |
| Forget only non-durable (703) and power lost: approval resurrected | approval; helper on `revoked-bonds` | revoked wins: not admitted, counted as unadmitted (diagnostic `bond_reconcile_unadmitted`); ForgetPhone completes |
| Demotion's renames unsynced and power lost (finding 4) | managed entry back, maybe without history | the restarted helper admits nothing before reconciliation, which demotes again and reports success only once durable |
| Helper restarts at any point | any of the above | reconciled false: no services, no reconnect, no enrolment, no cleanup until the receiver answers |

Receiver-side revocation records for wireless bonds are not needed for correctness any
more. ForgetPhone still tries `RevokePeerV1` at once (the fast path), and #113's bounded
retry loop remains for the source-only cases (helper source retirement) and the AP leg.
A successful reconciliation marks every bond leg whose identity the store no longer
approves complete (the helper owns the removal durably), so no bond record can pin the
256 slots and block pairing or revocation (finding 3).

## Bond states

Every bond the helper knows is in exactly one state:

| State | How it is entered | Persisted as | Services / reconnect | Cleanup |
|---|---|---|---|---|
| unaccepted | an enrolment window opens for the observed identity (only after the run's first reconciliation) | entry in `pending-removals` (written before the window opens) | refused | removed when the window closes |
| provisional | the owner accepted the number, the helper verified the exact bond and the receiver acknowledged it (`PairingBondAcknowledgedV1`) | the same `pending-removals` entry, kept; the acknowledgement is an in-memory flag | refused | when the window closes without a commit the bond is parked: never removed by cleanup, decided by the next reconciliation |
| admitted | the commit after the receiver's durable trust write, or a reconciliation that finds the approval | `managed-bonds` with a confirmed directory sync (#117), and **then** the `pending-removals` entry durably removed | allowed (Android Auto RFCOMM, HFP, `ReconnectPeerV1`) once reconciled | never removed while approved (#74) |
| revoked | `RevokePeerV1` (ForgetPhone, a retraction), or reconciliation of an identity the store no longer approves | moved from `managed-bonds` to `revoked-bonds` (history first, then removal from managed, then the removal owed in `pending-removals`; each durable) | refused | removed by the revocation or by cleanup of the owed entry; history is revocation authority only |

Admission is checked by one predicate: reconciled in this helper run, on `managed-bonds`,
durably synced, not on `pending-removals`, not parked and not being demoted. Cleanup follows
`pending-removals`, but never before the first reconciliation, never for a parked bond, and
never for a bonded device the receiver approves. A `managed-bonds` or `revoked-bonds` entry
for the same adapter/address never suppresses a removal (finding B: a forgotten phone's
history is never taken as acceptance of a new bond). An identity that is currently on
`managed-bonds` cannot open a new enrolment window; it must be forgotten first.

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
receiver retires the enrolment's helper source (Invalidate, tracked by #113's bounded
retry loop if it fails), which closes the window and parks the bond, and requests a
reconciliation; the store does not approve the bond, so the helper removes it. If the
receiver process exits at this boundary (finding A), the helper's identity-loss cleanup
parks the bond and the restarted receiver's reconciliation removes it.

A failed or lost commit after a durable write keeps the approval: the store is the
authority. The receiver emits `pairing_commit_failed`, retires the enrolment's source
(the helper parks the bond, no services) and reconciles, which admits the bond. A write
that landed only as a committed-but-not-durable store result (703) is never committed;
the receiver retracts it through the same path as `ForgetPhone` (store forget plus
tracked `RevokePeerV1`) and reconciles. A receiver crash after its store write and
before the commit is healed by its start-up reconciliation (crash window 1).

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
loads. Helper lease retirement removes unaccepted devices through
BlueZ RemoveDevice and parks provisional ones; a failed removal stays queued for retry on
helper ticks, even if BlueZ temporarily disappears. The queue is persisted next to
`managed-bonds` as `pending-removals` (same root-owned directory, owner-only regular file,
no-follow traversal, exclusive staging, fsync and atomic rename, at most 256
records) and an entry is written before any window opens, so a helper restart
cannot lose a removal; an admission whose entry cannot be written is refused.
Rebuilding the queue from BlueZ was rejected: BlueZ cannot distinguish a bond an
enrolment window created from one the owner made, and removing the latter would be
destructive; reconciliation acts only on identities the helper's files name. The
list is not revocation authority on its own once its entry is gone. A removal that
succeeded but whose record cannot be rewritten is remembered in memory so a device
later paired again at that address is not removed on every tick. `revoked-bonds`
uses the same file rules and, when full, evicts its oldest entry. Admitted bonds
retain main's CancelPairing guards. No production state or installed helper is
exercised by host tests.

The helper RPC is a versioned extension: an older installed helper fails closed (it
refuses `ReconcileBondsV1`, so the receiver keeps retrying and a newer helper with an older
receiver admits nothing).
The host suite does not qualify phone pairing, controller key persistence, HFP
interoperability, AP association or live wireless projection. Issue #72 remains
open: its persisted-key/controller reconnect failure needs separate live evidence.
