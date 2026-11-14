# Enrolment ordering

`ConfirmPhonePairing` opens helper admission. For the production wireless helper,
this is an in-memory pending enrolment, not approval. The consumer must retrieve
the bound six-digit challenge and explicitly accept its one-time token. The
receiver then polls `PairingBondConfirmedV1`: the helper verifies that the exact
captured BlueZ owner, device and adapter incarnations still exist and are bonded.
The helper persists its managed-bond ownership only after this explicit acceptance
and exact bond verification; pending admission does not create managed-bonds state.
Only a successful bond acknowledgement permits the shared `persist_pairing`
routine to call `PhoneDirectory::pairing_confirmed`. The non-comparison flow
continues to use that same persistence routine after helper confirmation (M10).
No deadline or background callback supplies acceptance.

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
if BlueZ temporarily disappears. Accepted, acknowledged bonds retain main's
CancelPairing guards. An abrupt receiver exit triggers helper identity-loss
cleanup. No production state or installed helper is exercised by host tests.

The helper RPC is a versioned extension: an older installed helper fails closed.
The host suite does not qualify phone pairing, controller key persistence, HFP
interoperability, AP association or live wireless projection. Issue #72 remains
open: its persisted-key/controller reconnect failure needs separate live evidence.
