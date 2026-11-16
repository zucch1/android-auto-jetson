# Enrolment ordering

## Authority rule

Two processes keep durable bond state: the receiver's trust store
(`approved-phones.json`) and the helper's `managed-bonds`, `pending-removals` (the
enrolment journal; the name is historical) and `revoked-bonds`. They cannot be committed atomically together, so exactly one of them
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
| on `revoked-bonds`, a fresh enrolment of it acknowledged in this helper run and awaiting its commit | admitted: history durably cleared, then `managed-bonds` synced, then the journal entry removed (the commit's own order) | the journal entry is released; the bond is kept and refused |
| on `revoked-bonds` otherwise (no committed enrolment since its revocation) | stays revoked: demotion completed, **not** admitted (#74); counted as unadmitted (diagnostic `bond_reconcile_unadmitted_enrol_again`); a bonded device is logged (`bond_kept_revoked_identity`, #136); a stale journal entry of an unaccepted window is dropped | stays revoked and refused; a bonded device is **kept** and logged (`bond_kept_revoked_identity`, #136) |
| on `managed-bonds` | admitted; a journal entry left beside it (commit stopped between its writes) is durably removed | demoted: `revoked-bonds`, then out of `managed-bonds`, each durable; refused from then on, its bond **kept** (#136) |
| on `pending-removals` with a bonded device | admitted: `managed-bonds` synced, then the journal entry removed (the approval landed, the commit did not) | entry released, bond **kept** and refused, `bond_kept_unowned_enrolment` logged |
| on `pending-removals`, no bonded device | counted as unadmitted; the entry is released | entry released |
| nothing | counted as unadmitted (approved, but no bond to admit) | an unmanaged bond is never touched |

**The Jetson's Bluetooth adapter is shared** (owner decisions of 2026-11-15 on #136; they
replace the 2026-11-14 "Android-Auto-exclusive" policy). Android Auto shares the adapter with the
owner's other Bluetooth use and, later, the infotainment system, so **Android Auto never deletes
or cancels a Bluetooth pairing**: the helper sends no `Adapter1.RemoveDevice`, no
`Device1.CancelPairing` and no `Device1.Disconnect`, ever. That includes the end of an Android Auto
enrolment window that is cancelled, rejected, times out or fails, and every restart. BlueZ has no
generation-conditional delete: the helper cannot prove that the pairing it would delete or cancel
is still the one it decided about, and every scoped variant tried (#140, then two reviews of #143)
kept a race against a pairing made outside Android Auto at the same moment, a retry after the
window closed or after a restart, or `CancelPairing` turning into an unpair of pre-existing state.
The only provable rule is never to delete. Instead:

- A phone forgotten in Android Auto (`ForgetPhone`, `RevokePeerV1`), retracted, or no longer in
  the store's approved set is demoted to `revoked-bonds` and stays paired in BlueZ. Only the
  Android Auto connection the helper itself holds for that phone (its RFCOMM socket and handoff) is
  closed.
- An enrolment window that ends without a commit (cancel, reject, timeout, adapter setup failure,
  consumer or receiver loss, a BlueZ restart, the device object disappearing) rejects a pending
  numeric comparison through the helper agent's reply (`org.bluez.Error.Rejected`), closes
  `Pairable`/`Discoverable` and releases its journal entry. A pairing it leaves behind (one the
  window made, one that existed before, or one an outside pairing completed meanwhile) stays paired.
  An acknowledged bond is parked instead until the receiver's reconciliation decides; if the
  receiver does not approve it, it is released the same way.
- A pairing left behind and a revoked identity, whether its old bond is kept or it was paired again
  outside Android Auto, are refused on every Android Auto path: no admission, no Android Auto RFCOMM
  bootstrap (`NewConnection`), no `AuthorizeService` for Android Auto or HFP, no
  `ReconnectPeerV1`. Only a fresh enrolment that reaches its commit admits it (and clears a
  revocation).
- Each such pairing is logged with the existing redaction (`id-` plus a digest keyed per helper run,
  stable within the run, never the raw address). Every reconciliation that finds a revoked
  identity still bonded logs `system-helper: warning bond_kept_revoked_identity <redacted address>:
  this phone was forgotten in Android Auto but is still paired in Bluetooth; the Bluetooth adapter
  is shared, so its pairing is kept and Android Auto refuses the phone. Enrol the phone again in
  Android Auto to use it.` A released journal entry whose device is still bonded logs `system-helper:
  warning bond_kept_unowned_enrolment <redacted address>: this phone is paired in Bluetooth but its
  Android Auto enrolment did not complete; ...` (when the window closes, or at the reconciliation
  that releases it). An entry released at a BlueZ restart or a removed device object is not logged
  (no inventory is read then).
- The pairing stays until the owner removes it in Bluetooth settings, or the infotainment system
  does under its future system contract (where one side owns the adapter); bond deletion is
  deferred to that contract.

`pending-removals` is now only the enrolment journal: an entry means an enrolment of the identity
was opened and has not yet committed or been released, so the identity is never admitted, and after
a restart the receiver's reconciliation decides (an approval whose commit never landed is admitted;
anything else is released). It is never a removal obligation. Entries written by an older,
exclusive-adapter helper (whose demotions journalled a removal) are read the same way: released,
the bond kept and refused, never acted on destructively. The file keeps its name so those entries
are read; no migration step is needed.

Reconciliation reports success only when every demotion and every admission is durable; otherwise
it fails and the receiver retries. A journal entry whose release cannot be written stays (it only
refuses) and is released by a later reconciliation.

Revoked versus approved: history is cleared only when a fresh enrolment of the identity
reaches the committed state: the owner explicitly accepted its number, the helper verified
and the receiver acknowledged the bond, and the receiver's durable approval arrived (its
`PairingBondCommittedV1`, or, after an acknowledged enrolment whose commit never landed, the
next reconciliation of this helper run that finds the approval). The commit clears history
first, then writes `managed-bonds`, then removes the journal entry. Opening, rejecting,
cancelling or timing out an enrolment never clears it. So an approved identity still on
`revoked-bonds` has no commit after its revocation: it is a resurrected (non-durable forget
lost to power loss) or stale approval, and the revocation wins. The window's journal entry is
dropped once the window closes (the approved bond stays, #74; history keeps the refusal),
so the owner can enrol the phone again at once, or ForgetPhone it (it then
completes). A committed approval always wins, because its commit cleared the history. An
acknowledged re-enrolment whose commit never landed and whose helper also restarted is not
admitted (the acknowledgement is in memory only); it is counted as unadmitted and the owner
enrols again.

Until the first successful reconciliation of a helper run nothing is admitted for services
or reconnect, no enrolment window opens, and no journal entry left by an earlier run is
released, because any journal entry may be an approval whose commit never landed; the receiver's
answer decides. Every helper state change confirms persistence (rename plus directory fsync)
before it reports success; on any failure reconciliation reports failure, the helper keeps the
stricter state (admission stays off for this run), and the receiver retries on its next pacing
step.

## Crash windows

| Window | Durable state left behind | How reconciliation heals it |
|---|---|---|
| Receiver exits after the acknowledgement, before its trust write | journal entry, bond; no approval | the helper parks the bond (not admitted); the restarted receiver's set lacks it: entry released, bond kept and refused |
| Receiver's trust write is durable, it exits before the commit (window 1) | approval, journal entry, bond | parked; the restarted receiver's set has it: admitted, approval kept |
| Commit sent, reply lost, or the helper's commit write failed | approval, journal entry (maybe also `managed-bonds`), bond | the receiver keeps its durable approval, retires the enrolment source (which parks the bond) and reconciles: admitted |
| Helper wrote `managed-bonds`, crashed before removing the journal entry (window 2) | approval, managed and journal entries | the restarted helper admits nothing; the live receiver sees the new incarnation and reconciles: journal entry dropped, admitted, bond kept |
| Trust write fails after the acknowledgement | journal entry, bond; no approval | the receiver revokes it (`RevokePeerV1` releases the entry) or its reconciliation releases it; the bond is kept and refused; no receiver record needed |
| Approval only committed-but-not-durable (703) | approval (maybe lost), journal entry, bond | the receiver retracts (store forget, tracked revocation) and reconciles; if the retraction cannot be written either, the approval remains and reconciliation keeps the helper consistent with it; after a power loss the next start's reconciliation decides from what survived |
| ForgetPhone durable while the helper is down, then the receiver restarts (finding 2) | no approval; helper still on `managed-bonds` | the receiver's record is lost but not needed: its start-up reconciliation demotes and refuses it, the bond kept (#136); the phone can enrol again |
| Forget only non-durable (703) and power lost: approval resurrected | approval; helper on `revoked-bonds` | revoked wins: not admitted, counted as unadmitted (diagnostic `bond_reconcile_unadmitted_enrol_again`); ForgetPhone completes |
| Demotion's renames unsynced and power lost (finding 4) | managed entry back, maybe without history | the restarted helper admits nothing before reconciliation, which demotes again and reports success only once durable |
| Demotion stops after history (managed entry not yet dropped) | managed and history entries, bond | the restarted helper admits nothing; reconciliation demotes again; the bond is kept (#136) |
| Demotion stops after the managed entry is dropped | history entry, bond | refused; the bond is kept (#136) |
| Demotion left a journal entry by an older (exclusive-adapter) helper | journal entry, maybe managed or history entries, maybe a bond | reconciliation demotes a managed entry, releases the journal entry without any BlueZ call, keeps and logs the bond |
| Helper stops or crashes with an enrolment window open | journal entry (an orderly stop releases it at once), maybe a bond | reconciliation releases the entry unless approved; the bond is kept, refused and logged |
| A leftover `window-created-bonds` file (a pre-release build of #143) | that file | never read; ignored |
| Demotion stops after history **and** the store's forget was lost (double fault) | approval resurrected; managed and history entries, bond | the revocation wins: not admitted, counted as unadmitted; ForgetPhone again completes the demotion |
| Helper restarts at any point | any of the above | reconciled false: no services, no reconnect, no enrolment, no release until the receiver answers |

Receiver-side revocation records for wireless bonds are not needed for correctness any
more. ForgetPhone still tries `RevokePeerV1` at once (the fast path), and #113's bounded
retry loop remains for the source-only cases (helper source retirement) and the AP leg.
A successful reconciliation marks every bond leg whose identity the store no longer
approves complete (the helper keeps every such bond refused and never removes one, #136), so no
bond record can pin the 256 slots and block pairing or revocation (finding 3).

## Bond states

Every bond the helper knows is in exactly one state:

| State | How it is entered | Persisted as | Services / reconnect | Bond in BlueZ |
|---|---|---|---|---|
| unaccepted | an enrolment window opens for the observed identity (only after the run's first reconciliation) | entry in `pending-removals` (written before the window opens) | refused | kept when the window closes; the entry is released |
| provisional | the owner accepted the number, the helper verified the exact bond and the receiver acknowledged it (`PairingBondAcknowledgedV1`) | the same `pending-removals` entry, kept; the acknowledgement is an in-memory flag | refused | when the window closes without a commit the bond is parked and the next reconciliation decides: admitted, or released and kept |
| admitted | the commit after the receiver's durable trust write, or a reconciliation that finds the approval | `managed-bonds` with a confirmed directory sync (#117), and **then** the `pending-removals` entry durably removed | allowed (Android Auto RFCOMM, HFP, `ReconnectPeerV1`) once reconciled | kept |
| revoked | `RevokePeerV1` (ForgetPhone, a retraction), or reconciliation of an identity the store no longer approves | moved from `managed-bonds` to `revoked-bonds` (history, then removal from managed; each durable) | refused (also a bond paired again outside Android Auto) | kept (shared adapter, #136); history is refusal only, cleared only by a committed fresh enrolment |
| left behind | an enrolment window closed without a commit, its entry released | nothing (the identity is on none of the helper's files) | refused (not on `managed-bonds`) | kept |

Admission is checked by one predicate: reconciled in this helper run, on `managed-bonds`,
durably synced, not on `pending-removals`, not parked, not being demoted and not revoked. No
bond is ever removed, so no history can be mistaken for acceptance of a new bond (finding B):
only a fresh enrolment's commit admits it. An identity that is currently on `managed-bonds`
cannot open a new enrolment window; it must be forgotten first. A phone whose pairing is kept
must also be unpaired (by the owner) before a new enrolment can show a number.

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
method). The helper re-verifies the exact bond, durably clears any revocation history of
the identity, writes `managed-bonds` with a confirmed directory sync and then durably removes
the journal entry; only now are services and
reconnect allowed. The non-comparison flow continues to use the same persistence
routine after helper confirmation (M10).

A failure before the acknowledgement closes the enrolment; the helper releases its journal
entry and keeps any bond, refused. A failure after it and before the commit (a failed or lost
acknowledgement, a deadline, owner or reload change in the acknowledgement window, or the
trust-store write itself) leaves a provisional bond with no store entry. The receiver retires
the enrolment's helper source (Invalidate, tracked by #113's bounded retry loop if it fails),
which closes the window and parks the bond, and requests a reconciliation; the store does not
approve the bond, so the helper releases it and keeps it refused. If the receiver process exits
at this boundary (finding A), the helper's identity-loss cleanup parks the bond and the
restarted receiver's reconciliation releases it.

A failed or lost commit after a durable write keeps the approval: the store is the
authority. The receiver emits `pairing_commit_failed`, retires the enrolment's source
(the helper parks the bond, no services) and reconciles, which admits the bond. A write
that landed only as a committed-but-not-durable store result (703) is never committed;
the receiver retracts it through the same path as `ForgetPhone` (store forget plus
tracked `RevokePeerV1`) and reconciles. A receiver crash after its store write and
before the commit is healed by its start-up reconciliation (crash window 1).

`RevokePeerV1` accepts an admitted, revoked or journalled identity and never makes a BlueZ call
(#136): it closes the identity's open window or Android Auto connection, demotes a managed
identity to history and releases its journal entry, each durable before it reports success. The
bond stays paired, refused. Every other identity is refused.

No deadline or background callback supplies acceptance.

Services (`AuthorizeService` for HFP and the Android Auto RFCOMM channel, and the
profile's `NewConnection`) and `ReconnectPeerV1` require the admitted state, so they
are refused while the bond is unaccepted or provisional and are allowed only after the
commit and its confirmed `managed-bonds` sync (finding C). An enrolment that fails to complete
leaves no Android Auto service authorized, so its phone cannot use Android Auto or HFP via
Android Auto over a link that stays up; the helper does not disconnect it (shared adapter, #136).

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
loads. Helper lease retirement releases unaccepted enrolments (their bonds kept, refused) and
parks provisional ones. The journal is persisted next to `managed-bonds` as `pending-removals`
(same root-owned directory, owner-only regular file, no-follow traversal, exclusive staging, fsync
and atomic rename, at most 256 records) and an entry is written before any window opens, so a
helper restart cannot lose an enrolment whose approval may have landed; an admission whose entry
cannot be written is refused. The list is not revocation authority on its own once its entry is
gone. `revoked-bonds` uses the same file rules and, when full, evicts its oldest entry. The helper
never calls `CancelPairing` (BlueZ turns one without an in-flight `Pair()` into an unpair of the
device's existing keys). No production state or installed helper is exercised by host tests.

The helper RPC is a versioned extension: an older installed helper fails closed (it
refuses `ReconcileBondsV1`, so the receiver keeps retrying and a newer helper with an older
receiver admits nothing).
The host suite does not qualify phone pairing, controller key persistence, HFP
interoperability, AP association or live wireless projection. Issue #72 remains
open: its persisted-key/controller reconnect failure needs separate live evidence.
