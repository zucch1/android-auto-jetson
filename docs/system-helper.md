# Task 34: reference privileged system helper (host reference)

## Scope and trust boundary

`aa-system-helper` is a C++20 daemon using the existing POSIX D-Bus wire stack
and OpenSSL Crypto. It handles no media, Android Auto protocol, arbitrary
commands, network profile strings, phone identifiers, or secrets. Tasks 35/36
must supply qualified NetworkManager/BlueZ effects later. The production
`UnavailableBackend` returns unavailable status and `helper_unavailable` for
effects, including release/profile/pairing; it never reports invented success.
USB policy and ModemManager exclusion are status seams, not policy writers.

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
This mode cannot be selected by root and still uses the unavailable backend.
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
requires callers to have finished. Existing
wired clients without this dependency retain their original behavior; they do
not gain helper authority. The actual `ControlService::confirm_phone_pairing`
path invokes it only after registered-consumer UID/name authorization,
`PairingGate::confirm`, and `PhoneDirectory::pairing_confirmed` all succeed.
`PairingOpened` is emitted only after the bridge succeeds. Listening for or
forging that signal never mints grants. No session D-Bus method proxies Issue.

A helper error closes the control gate and invalidates the source; trust may
already be persisted and is **not rolled back**. The pre-existing
`trust_store_not_durable` approval outcome never calls the bridge. Cancel,
consumer teardown/owner loss, successful or committed-not-durable forget, and
control-service shutdown invalidate helper authority. Forget conservatively
invalidates all helper grants/leases. Failed remote invalidation also discards
local grants; the helper independently expires grants and detects receiver loss.
Control-service owner refresh runs at least once per second even under traffic.
Typed effect requests may carry explicit consumer-generation/confirmation IDs;
the backward-compatible zero/default selects only the bridge's current active
source. Grants are selected by source and action together, never action alone.
A source-scoped lease cannot be replaced by another confirmation's effect
until cleaned; cancelling an unrelated source does not clean the owning lease.

`BusClient::reconnect(TransportIdentity)` performs the actual receiver-side
`ApprovedPhoneStore::lookup` of the validated **full exact identity** before
calling the helper. Unknown, mismatched, unusable-store and forgotten identities
fail closed. There is no public approved boolean. No phone identity crosses
the helper bus API. The helper trusts this check only because of the receiver
executable/process binding, not because it independently has the user store.

## System bus API 1.0.0

Name `org.custom.AndroidAutoSystemHelper`, path
`/org/custom/AndroidAutoSystemHelper`, interface
`org.custom.AndroidAutoSystemHelper1`. XML is in `docs/dbus/` (reference schema;
the helper does not expose Introspectable). Methods:

| Member | In | Out | Meaning |
|---|---|---|---|
| Issue | `ttuu` generation, confirmed source, action, channel | `ut` error, grant | Trusted receiver bridge only |
| Execute | `uut` action, channel, grant | `ut` error, state | Strict typed action dispatch |
| Invalidate | `tt` generation, source | `ut` error, reserved zero | Zero selects all generations/sources |

Errors: 0 success, 1000 unavailable, 1001 rejected, 1002 protocol mismatch.
Nonzero errors carry value zero. States: 0 inactive, 1 active, 2 unavailable.
Unknown interfaces/paths/members/signatures fail closed. Non-method traffic is
dropped. Method replies are matched by serial, daemon-stamped helper unique
sender and destination; name handover cannot redirect an in-flight call.

Actions (wire numbers are append-only):

| Number | Action | Authorization argument |
|---|---|---|
| 1 | AP acquire | channel exactly 36; one-use AP grant |
| 2 | Open pairing (fixed backend 60-second-window contract) | channel 0; distinct one-use pairing grant |
| 3 | AP release | channel 0, grant 0, same receiver binding |
| 4 | AP status | channel 0, grant 0, same binding |
| 5 | USB policy status | channel 0, grant 0, same binding |
| 6 | ModemManager exclusion status | channel 0, grant 0, same binding |
| 7/8 | BlueZ register/unregister fixed AA profile | channel 0, grant 0, same binding |
| 9 | Approved reconnect mediation | channel 0, grant 0, receiver-side store lookup |
| 10 | Close pairing | channel 0, grant 0, same binding |

Issue grants are random server-side IDs distinct from confirmation IDs and
bound to the pinned receiver, consumer generation, confirmed source, action,
channel and steady-clock expiry (5 seconds). AP and pairing cannot share a
grant. Duplicate source/action issuance fails even after consume, cancel,
expiry or loss. Pending grants are capped at 16; lifetime replay history at
4096. Exhaustion fails closed, never evicts history, and requires operator
restart. All grants are consumed before revalidation/backend effects; backend
errors and ambiguous timeouts do not permit retries with the same grant.

## Loss handling and bounds

One receiver owns helper authority at a time. A steady-clock sweep scheduled
every second runs regardless of incoming traffic. Receiver unique-name loss,
death, UID/starttime/digest change and system-bus failure invalidate pending
grants and clean the stored owning backend lease internally (no attempt to
authenticate a dead process). Graceful helper destruction also cleans leases.
Test backends are nonblocking; future production backends must provide bounded
cleanup and persist/reconcile external leases across helper SIGKILL/crash.
There are no real external leases to recover in this reference backend.

Credential RPCs share an absolute 500 ms deadline and helper export read/reply
steps 500 ms. Typed client calls use the caller's timeout capped at one second;
confirmation's two Issue RPCs, status and reconnect each use a one-second budget.
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

Real AP, USB-policy verification, ModemManager exclusion, BlueZ pairing/profile
effects, target service/sandbox integration and Jetson RSS/timing qualification
are **deferred**. Host fixture success is not wireless or target acceptance.

The synthetic cross-smoke sysroot does not contain OpenSSL development inputs.
Like the TLS qualification targets, the helper is explicitly unavailable in
that configuration; native host helper tests remain mandatory. The cross-smoke
configuration regression verifies that no target OpenSSL lookup is attempted,
not that the helper has been cross-built or qualified on ARM64.
