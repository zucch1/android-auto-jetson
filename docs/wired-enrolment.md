# Wired enrolment (#133)

Approving a new wired phone through the receiver's authenticated D-Bus control
contract (`org.custom.AndroidAutoReceiver1`). Before #133, wired approvals were
written into `approved-phones.json` outside the product.

The flow follows the wireless `RequestAddPhone` / `ConfirmPhonePairing` shape,
so one consumer UI (the infotainment Qt/QML UI is the intended registered
consumer) drives wired and wireless enrolment the same way. Nothing is ever
auto-approved. A successful AOA switch is never trust. A phone becomes a
candidate only when admission has refused it.

## Contract sequence (consumer side)

1. `RegisterConsumer(s)`: the caller owns the consumer name and runs as the
   session user. Every step below is refused for any other caller.
2. `RequestAddPhone() -> t`: returns a one-time request id and emits
   `PairingRequest`. The id is bound to a snapshot of the identity of the
   attempt admission last refused. It fails with `trust_unknown_phone` if
   nothing was refused, or if the refusal is older than 120 s. It fails with
   `trust_port_only_requires_opt_in` for a serial-less phone, which this flow
   never grants.
3. `GetPairingCandidate(t) -> a{ss}`: returns `transport` (`wired`), `label`
   and `strength` (`serial-backed`). The label is the receiver's per-process
   pseudonym, and the receiver's refusal log line shows the same value as
   `phone_id`. It never carries a raw serial, AOA string or port. Only the
   consumer holding the request can call it, and nothing is broadcast.
4. Show the label and ask a human yes or no. A yes is `ConfirmPhonePairing(t)`.
   A no is `CancelPhonePairing(t)`, which writes nothing.
5. A confirm before step 3 is refused (`trust_rejected`) and the request
   closes: an unseen phone is never approved. A confirm after the 60 s window
   meets a request that has already closed (`PairingClosed`). The id is one-use
   either way.
6. A confirmed yes writes the bound identity to the store durably
   (`ApprovedPhoneStore::approve`, atomic replace) and emits `PairingOpened`.
   Close the window afterwards with `CancelPhonePairing(t)`; the approval stays.

Consumer loss (name owner change, `UnregisterConsumer`) closes every
outstanding request without writing anything.

## Receiver side

`aa-wired-receiver --wired-enrolment` (requires the bus, so not `--no-bus`):

- If admission refuses the phone (`transport_unauthorized_peer`), the receiver
  logs the usual refusal line and then
  `{"event":"wired_enrolment_open","phone_id":"<label>","window_ms":120000}`.
  It then serves the contract on the store-owner thread (`app::WiredEnrolment`
  is the `PhoneDirectory`).
- On an approval it logs `wired_enrolment_approved` and exits 0. The refused
  link is not reused: the phone is still in accessory mode, so its captured
  descriptor ids no longer match. Replug the phone (or reset its USB functions)
  and start the receiver again; the approved phone is then admitted on any port
  (serial-backed match).
- With no approval inside the window (or `--duration-ms`), it logs
  `wired_enrolment_closed` and exits 3, as without the flag.
- If admission succeeds, enrolment closes before the session starts. Pairing
  requests are then refused again, and `ForgetPhone` goes through the session
  path.

Without `--wired-enrolment` the receiver behaves as before: a refusal exits 3
and the bus refuses `RequestAddPhone`.

## Reference consumer: `aa-wired-enrol`

`aa-wired-enrol [--consumer-name NAME]` is for tests and owner bring-up. It
registers, requests, shows the candidate and asks `Approve this phone? [y/N]`
on the terminal. Only `y` or `yes` approves. An empty line, any other text,
EOF, or no answer within 50 s declines. Exit codes: 0 approved, 1 declined,
2 usage, 3 failed. It must run as the receiver's user on the same session bus.

Owner bring-up:

```sh
aa-wired-receiver --port <port> --wired-enrolment ...   # plug the phone in
aa-wired-enrol                                          # second terminal: check the label, answer y
# replug the phone, start the receiver again (without --wired-enrolment if preferred)
```

Wireless trust is unchanged. Wireless enrolment keeps its numeric-comparison
flow (`docs/task36-bluez.md`).
