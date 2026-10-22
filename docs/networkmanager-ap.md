# Task35: dedicated NetworkManager AP (host reference)

## Provisioning and fixed policy

Production `aa-system-helper` constructs both the NetworkManager and BlueZ backends
behind an explicit operation router. USB policy mutations and ModemManager mutations
remain unavailable. Existing executable/UID/PID/starttime authentication, action
numbers, source ownership, one-use grants and their short RPC budgets are unchanged.

In addition to the identity files in [system-helper.md](system-helper.md), an
operator must provision `/etc/android-auto-receiver/ap.conf` as a root-owned,
single-link regular file with no group/other permissions. Every directory
component must be root-owned, non-symlink and not group/other writable. The only
accepted schema has five whitespace-delimited fields, with no extra fields:

```text
v1 wlan0 US jurisdiction-confirmed 5277
```

The example in `packaging/ap.conf.example` intentionally says
`jurisdiction-unconfirmed` and is **not loadable**. The installer/operator must
explicitly confirm the actual jurisdiction (or replace US with the confirmed
two-uppercase-letter jurisdiction), interface and projection port. No country
is inferred, no automatic regulatory override is made, and there is no fallback
channel. Missing/malformed/unconfirmed configuration aborts daemon startup with
an actionable diagnostic. Configure the receiver's later projection listener to
the same port; task35 does not implement that listener or task37 handoff.

`StateDirectory=aa-system-helper`, mode0700, gives only
`/var/lib/aa-system-helper` writable state. `ap.credentials` is generated using
OpenSSL `RAND_bytes`: an 8-byte random SSID suffix and a 32-byte random WPA2 PSK,
both hex-encoded. The single credential file is0600, verified by anchored
`openat`/`O_NOFOLLOW`, owner/mode/type/link-count/size checks, exclusively staged,
fsynced, atomically renamed and directory-fsynced. Secrets are never logged or
returned by the control API. Root is the required production owner; the
non-root test mode substitutes the explicit fixture UID only. Invalid existing
credentials are refused, never overwritten as a recovery shortcut. A leftover
exclusive `ap.credentials.new` after a crash requires operator inspection and
removal of that specific root-owned staging file before retry; startup fails
closed rather than discarding unknown state.

## Lifecycle and isolation

The provisioned device must be an NM Wi-Fi device with the exact interface name,
no pre-existing active connection, and the only `iw dev` interface. This is
deliberately conservative: no simultaneous infrastructure or P2P interface is
accepted. `iw dev IFACE info` identifies the actual wiphy; `iw reg get` must
report the explicitly confirmed country for every reported regulatory domain;
that wiphy's `iw phy phyN info` must advertise5180MHz/channel36 without disabled,
no-IR or radar restrictions. Unknown/missing/mixed country and unavailable
channel evidence fail closed before profile activation. Regulatory eligibility
is rechecked at least once per second while active; actual channel is monitored
on every worker sweep. A CSA to44 is a failure, never a fallback.

Before NM activation, an atomic `nft -f -` transaction installs the exclusively
reserved `inet aa_receiver_ap` table, tagged `aa-system-helper-v1`. On the fixed
AP interface it permits only client DHCP68->67 and the fixed projection TCP
port, then drops all other host input (including DNS, SSH and IPv6 unrelated
services). Forwarding from **and to** that interface is unconditionally dropped
at priority-200, before NM's shared forwarding rules. NM shared mode supplies
the actual DHCP service; its DNS/NAT/forwarding defaults are not trusted. The
rules do not flush/replace any other table or change the host's global
forwarding sysctls. Other host firewall chains may still deny DHCP/projection;
real effectiveness, lease acquisition and deployment compatibility require
target qualification.

The private GIO adapter calls the documented
`AddAndActivateConnection2(a{sa{sv}},o,o,a{sv})->(o,o,a{sv})`. The fixed profile
has a helper-specific ID/UUID, no autoconnect, the provisioned interface,
SSID `ay`, mode`ap`, band`a`, channel36, WPA-PSK, RSN-only/CCMP-only,
IPv4`shared`/never-default, IPv6`disabled`, `persist=volatile` and
`bind-activation=dbus-client`. Returned settings/active paths are retained;
the NM unique owner is pinned. No arbitrary profiles, settings, command paths
or shell input arrive from the receiver.

A dedicated single worker owns all platform state; callbacks needed to open
GIO connections use its serviced local context. Commands and D-Bus calls are
bounded (500ms/300ms respectively); GIO connection establishment is cancellable
at300ms. The startup deadline is five seconds from the start of the attempt;
success is published only after NM State2 and an actual `iw` AP/channel36 check
before that deadline. The short control RPC queues work: pending acquire returns
state0 and `Response.applied=false`; callers must poll AP status. State1 is
verified active, never simply an accepted NM request. No new wire action/state
numbers were added.

Cleanup/release increments an ownership generation and queues teardown. A
cancelled generation can never publish active. Deactivation uses only the
returned active handle; the returned settings handle is deleted when possible
(volatile NM settings also disappear on disconnect). If activation's result is
unknown, or the NM owner changes, closing the bound GIO client requests NM's
client-lifetime cancellation, but that alone is not treated as shutdown proof.
The helper retains deny-all quarantine for ambiguous activation/owner loss; a
replacement owner's initially inactive property cannot authorize rule removal
or successful revocation. Known activation teardown requires an acknowledged
DeactivateConnection under the original pinned owner followed by inactive
device verification. Explicitly rejected activation or cancellation before Add
can also clean up under that same owner. Failed known-handle cleanup is retried;
unknown activation/owner loss requires operator-supervised restart after NM and
radio state are confirmed stable. This intentionally prefers quarantine over
automatic recovery of ambiguous effects. Service stop makes one bounded final
attempt and retains protection if safe
shutdown cannot be proved. Release reports unavailable while pending, then
status becomes inactive after cleanup. Failed startup never automatically
retries exposure: release/cleanup and a fresh authorized source are required.

SIGKILL leaves protective rules in place; NM's client-bound volatile activation
ends when its helper connection disappears. On the next acquire the helper
reconciles only its reserved, correctly tagged table, only after verifying the
device is inactive, then installs fresh protection. A foreign/untagged table or
an already active device is never deleted/disconnected. The table name/comment
are an exclusive root administration contract, not a defense against a hostile
root administrator.

Recovery first successfully enumerates `nft list tables inet`; failure is not
treated as absence. An existing reserved table must then be readable and tagged.
Installation uses **create table**, not idempotent add, so a concurrently created
foreign table cannot be adopted by the install transaction.

Production subprocesses use `posix_spawn` with absolute `/usr/sbin/iw` or
`/usr/sbin/nft`, explicit argument vectors, bounded output and a minimal fixed
environment, never a shell. Stdin uses a bounded anonymous memory file, avoiding
pipe-write/SIGPIPE races. Deployment must provide tools at those exact paths.
The sandbox admits AF_NETLINK because these two tools use netlink; GDBus itself
uses AF_UNIX. No service, rules, tools or configuration were installed by this
task.

## Revocation V1 and composition with task36

`PhoneDirectory::take_revocation_v1()` transfers a copied old phone ID and full
transport identity captured **before deletion on the store-owning thread**.
`PairingPolicy` supplies that snapshot directly; `WiredPhoneControl` captures it
in its owner queue and transfers it under its existing mutex. No helper backend
reads the approval store or its file. There is no zero-argument success fallback;
real store adapters must supply the versioned snapshot.

After success or committed-not-durable error703, and never after a pre-commit
error, `ControlService` calls `Confirmation::forgotten_v1(snapshot, progress)`. The AP
`BusClient` calls authenticated `RevokeAP1(t phone_id)->(ut error,reserved)` on
the existing interface. Existing action IDs are untouched. The phone ID is a
bounded idempotency key; AP revocation is necessarily shared-credential-wide.
The full old identity remains in the receiver snapshot for exact BlueZ revocation,
not in NM settings or logs. It is not overloaded into the channel parameter.

Revocation fences pending startup, rotates credentials durably, and evicts all
AP access by deactivating the AP. There is no automatic restart with the new
PSK. Before rotation/deactivation it tightens the owned input chain to deny all
AP input, including projection, so failed NM shutdown retains quarantine rather
than leaving the old credential able to reach the listener. Unverified shutdown
remains an external failure even if quarantine succeeded. Ordinary Invalidate,
release, consumer teardown or receiver loss do not
rotate credentials. A bounded250ms helper wait may return unavailable while
revocation is pending; the backend retains and retries it, with no duplicate
rotation on the same in-flight/completed phone ID. Distinct phone IDs start
distinct rotations. External failures are reported and retained in a bounded
ControlService queue with the exact immutable old snapshot and per-leg progress; repeating the authorized
ForgetPhone for that ID retries external work without repeating store deletion.
Error703 remains703 on every retry; `phone_revocation_ap_pending` and
`phone_revocation_bond_pending` separately expose unfinished work. Acknowledged AP
success is not resent when only bond removal fails, preserving credential bytes.
Do not interpret703 as trust rollback. This retry queue is
process-memory state, not a durable outbox; after a receiver crash/restart an
operator must reconcile any separately failed external operation. NM cleanup
still occurs on receiver loss; persisted credential rotation is not reversed.

The composed daemon routes exact-peer bond revocation to BlueZ using the captured
old identity, never an arbitrary target or all bonds. One1000ms client attempt
budget covers both legs, capped at500ms each with a reservation for the second.
See [system-helper.md](system-helper.md) for ownership, precedence and recovery limits.

## Executable host proof and limits

```sh
cmake --build build/host-dev -j2
ctest --test-dir build/host-dev -R 'system_helper|trust|control_service|wired_e2e' --output-on-failure
build/host-dev/aa_nm_ap_test --gtest_filter='NmDaemon.HappyStartProjectionIsolationAndRollback:NmDaemon.CommittedForget703ReachesRealDaemonButPrecommitDoesNot:NmDaemon.OwnerLossCannotUseReplacementInactiveAsShutdownProof'
```

`aa_nm_ap_test` starts fresh private dbus-daemon instances and the **actual
aa-system-helper binary**, serves a GDBus NM double with the official signature,
and drives the real BusClient/authorization/backend chain. Only the privileged
`iw`/`nft` boundary is replaced with a framed AF_UNIX service; private mode
**never execs** privileged tools. Private mode requires an explicit fifth
command-socket argument for standalone NM, or explicit `--combined SOCKET` for
both adapters, and is refused for root or setuid processes. Four-argument private
mode supplies BlueZ only with AP unavailable. There is no environment switch
that enables fake commands in production.

See this worktree's `.omo/evidence/composition/implementation/` for composition
command transcripts and handoff. Host tests prove RPC/profile construction, sequencing, failure
behavior and filesystem policy, not a physical DHCP lease or kernel firewall.
RTL8822CE channel36, real five-second startup, phone join/revocation, effective
kernel firewall isolation, Jetson RSS and installed-service/sandbox behavior
remain **TARGET BLOCKED**. Task35 is not fully closed or target-accepted.
