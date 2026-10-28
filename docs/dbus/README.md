# Session D-Bus Control Contract

This directory holds the checked contract authority for the session D-Bus control
interface, `org.custom.AndroidAutoReceiver1.xml`. The tool
`tools/ipc/dbus_contract.py` validates that XML against the checked schema
(`tools/ipc/dbus_contract_schema.py`) and, in `generate` mode, embeds it into the
generated introspection header `aa/ipc/dbus_contract.generated.hpp`, which the
build produces through the `aa_dbus_contract_codegen` target and
`include/aa/ipc/Contract.hpp` serves as `aa::ipc::introspection_xml()`. The XML is
the checked contract authority: the generated header carries the same bytes, and
the build fails closed with named diagnostics on any drift.

## Task 21 Acceptance Limits

Task 21 delivers the checked contract and its in-process components, not a running
service. The following limits are explicit acceptance conditions for this task.

- Task 21 delivers the checked contract, the generated introspection, the
  in-process dispatch (`aa::ipc::ControlService`), the authorization semantics,
  and the typed event seams (`aa::ipc::ControlEvent`, `aa::ipc::ControlEventSink`).
  It does not deliver a persistent production-exported D-Bus service that owns the
  declared bus name `org.custom.AndroidAutoReceiver` and object path
  `/org/custom/AndroidAutoReceiver`.
- The contract's five signals each have a typed event representation (the
  `ControlEvent` variant members `StateChanged`, `PairingRequested`,
  `PairingOpened`, `PairingClosed`, `DiagnosticEmitted`), but no production D-Bus
  emitter is delivered in this task.
- Owner-loss reconciliation is lookup-on-use:
  `ControlService::refresh_consumer_owner()` re-resolves the registered consumer's
  owned name through the credential lookup and revokes authority when ownership is
  absent, different or unverifiable. Idle cleanup requires the future runtime
  (task 24) to drive that refresh while idle; this task delivers no autonomous
  `NameOwnerChanged` monitor.
- Pairing request ids are admitted against a bounded budget: 64 admissions per
  gate/service lifetime (`PairingGate::kIssuedBudget`), fail-closed when spent
  (`ipc_queue_full`). A one-time id is never re-admitted within the gate lifetime,
  including after cancel or close. Recovery requires ending the
  receiver/control-service lifetime and constructing a fresh service; consumers
  then re-register and discard old pending ids. Qualification: non-reuse is
  GATE-LIFETIME scoped. Random generation does not establish mathematical
  uniqueness across receiver lifetimes; do not read this as "an id can never
  repeat across all restarts". Removing the availability limit for a continuously
  running receiver is a separate future design that must preserve id
  unpredictability.

## Validation

- `tools/ipc/dbus_contract.py` runs in `check` (validate the checked XML) and
  `generate` (validate, then emit the introspection header) modes.
- Validation is a structural allowlist: exactly fourteen methods, five signals and
  one read-only property, with argument types restricted to scalars and the string
  map so bulk or high-rate payload shapes are rejected.
- Violations print named `DBUS_CONTRACT_*` diagnostics, exit nonzero, and produce
  no generated output, so the build fails closed.
- The embedding is byte-preserving: XML bytes are encoded as fixed-width octal
  escapes, and the generated header carries a compiled byte-length probe (a
  `static_assert` that the literal size matches the recorded byte count).
- `tests/dbus/contract_scenarios.py` holds the negative regression suite: mutated
  fixtures must fail closed, and embedding cases compile the generated header and
  byte-compare the compiled value against the fixture bytes.
