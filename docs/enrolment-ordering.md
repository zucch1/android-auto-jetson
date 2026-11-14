# Enrolment ordering

`ConfirmPhonePairing` opens helper admission. For the production wireless helper,
this is an in-memory pending enrolment, not approval. The consumer must retrieve
the bound six-digit challenge and explicitly accept its one-time token. The
receiver then polls `PairingBondConfirmedV1`: a verification only, which checks that
the exact captured BlueZ owner, device and adapter incarnations still exist and are
bonded after the consumed acceptance, and records nothing. Once it reports a bond,
the receiver runs every receiver-side check (deadline, consumer ownership, store
reload/snapshot) and only then asks the helper to record the bond with
`PairingBondAcknowledgedV1`; the helper writes its managed-bond ownership at that
call and nowhere else. Pending admission does not create managed-bonds state.
After the acknowledgement the receiver reconciles deadline, owner and snapshot once
more (the RPC can dispatch changes) and then calls the shared `persist_pairing`
routine, which calls `PhoneDirectory::pairing_confirmed`. The non-comparison flow
continues to use that same persistence routine after helper confirmation (M10).

A failure before the acknowledgement closes the enrolment and the helper removes
the unaccepted bond. A failure at or after it (a failed or lost acknowledgement, a
deadline, owner or reload change in the acknowledgement window, or the trust-store
write itself) leaves a bond the helper may manage with no store entry, so it is
retired through the existing tracked revocation record (`revocations_`, as
`ForgetPhone` uses it): bond leg pending, AP leg complete (nothing was ever
approved), plus the helper source retirement. The record has no phone id, so it is
retried from owner reconciliation (control calls and idle cycles, paced at two
seconds) rather than from `ForgetPhone`, it emits the existing
`phone_revocation_*_pending` diagnostics, and its captured identity blocks
re-enrolment until the bond is removed. The helper accepts `RevokePeerV1` for an
identity on its pending-removal list as well as a managed one, so a lost
acknowledgement reply cannot strand a recorded bond. A committed-but-not-durable
store result (703) is a failed write here, as in the non-comparison flow on this
base; main's #113 reworks that reconciliation and the helper source retirement and
was not merged when this was written.

No deadline or background callback supplies acceptance.

After the owner's acceptance the helper authorizes the peer's HFP service and the
Android Auto RFCOMM channel, which is before the trust-store write. Whenever the
enrolment then fails to complete, the helper disconnects the device (BlueZ
`Device1.Disconnect`) before it removes the bond, on both retirement paths
(unaccepted removal and `RevokePeerV1`), so a removal that keeps failing cannot
leave an unaccepted phone's HFP link connected. The disconnect is best effort and
is retried with each removal attempt.

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
loads. Helper lease retirement removes unaccepted managed devices through
BlueZ RemoveDevice; failed removal stays queued for retry on helper ticks, even
if BlueZ temporarily disappears. The queue is persisted next to `managed-bonds`
as `pending-removals` (same root-owned directory, owner-only regular file,
no-follow traversal, exclusive staging, fsync and atomic rename, at most 256
records) and an entry is written before any window opens, so a helper restart
completes a removal that was pending retry; an admission whose entry cannot be
written is refused. Rebuilding the queue from BlueZ was rejected: BlueZ cannot
distinguish a bond an enrolment window created from one the owner made, and
removing the latter would be destructive. The list is not revocation authority.
An entry leaves it when the bond is acknowledged (the receiver then owns the retirement)
or when its device is gone from BlueZ; a removal that succeeded but whose record cannot
be rewritten is remembered in memory so a device later paired again at that address is
not removed on every tick. Accepted, acknowledged bonds retain main's
CancelPairing guards. An abrupt receiver exit triggers helper identity-loss
cleanup. No production state or installed helper is exercised by host tests.

The helper RPC is a versioned extension: an older installed helper fails closed.
The host suite does not qualify phone pairing, controller key persistence, HFP
interoperability, AP association or live wireless projection. Issue #72 remains
open: its persisted-key/controller reconnect failure needs separate live evidence.
