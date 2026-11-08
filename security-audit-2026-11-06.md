# Security Audit — android-auto-jetson

**Date:** 2026-11-06
**Auditor:** automated multi-lane review (Orchestrator + specialist lanes), plus a subsequent
single-agent independent validation pass that re-verified the findings by *executing* the
production code (see §7).
**Audited revisions:**
- working tree `8369096` (detached HEAD, `aa/task27-trust-store`) — called **HEAD** below
- mainline `origin/main` `6e1ffc7` (35 commits ahead) — called **main** below, exported read-only to `/tmp/opencode/repo-main`

**Status:** read-only audit. No production source file was modified. The only repository changes
are this report and three added regression tests in `tests/trust/` (see §7.4).

---

## 1. Bottom line

The codebase is unusually disciplined for a pre-release project: fail-closed trust admission,
strict credential loading, bounded parsers, atomic trust-store writes, pinned dependencies,
and a small, explicit privileged surface. **No Critical finding and no remotely exploitable
code-execution bug was found.**

Two things dominate the residual risk, and neither is a memory-safety bug:

1. **Supply-chain trust is partly self-referential.** The documented pre-merge ARM64 relay
   builds the *untrusted PR head tree* on the maintainer's workstation with the maintainer's
   `gh` credentials and the private sysroot, then posts the trusted status. The CI gates,
   dependency hashes and provenance anchors also live in the PR-modifiable tree.
2. **The authorization boundary is "same Unix UID".** Both the trust store and the D-Bus
   control surface treat any process of the session user as the receiver. That is a legitimate
   appliance design, but it means one compromised same-UID process (browser, script, plugin)
   can approve phones, revoke phones, and drive projection.

Everything else is Medium/Low hardening: a pre-auth D-Bus message-size desync that can kill the
control service, a 60-second pairing window that is not bound to a user-visible identity,
approval persistence that is not transactional with the helper, acceptance of plaintext AAP
frames after TLS is active, in-memory Bluetooth bond bookkeeping that is lost on helper restart,
non-PIE/unhardened target binaries, and a long list of small robustness gaps.

The independent validation pass (§7) **confirmed** the headline findings and **added two Medium
defects that the first pass missed**, both in the trust store: a symlink at an intermediate path
component redirects the approved-phone store outside the state tree (M11), and a serial-less wired
approval aliases every serial-less device on that port (M12).

### Finding counts

| Severity | Count |
|---|---|
| Critical | 0 |
| High | 1 |
| Medium | 12 |
| Low | 16 |
| Informational | 8 |

Medium counts M1–M10 from the initial review plus **M11** and **M12** from the independent
validation pass. Every finding was independently re-checked; none were withdrawn.

---

## 2. Scope and method

### 2.1 Threat model used

| Actor | Capability assumed |
|---|---|
| Network / on-path | Can connect to the receiver's listening TCP port, tamper with TCP, replay records; cannot read the HU private key |
| Phone (paired or unpaired) | Sends arbitrary USB/TCP/TLS bytes, arbitrary AAP frames/protobuf, arbitrary media; controls its own identity claims |
| Local unprivileged user | Can run processes, open session D-Bus, connect to AF_UNIX sockets, send messages within bus policy |
| Local same-UID process | Everything the receiver's UID can do, including writing its state directory and speaking on its session bus |
| Malicious contributor | Can open a PR, modify any tree file including CI, CMake and validators; cannot push to protected branches |
| Compromised upstream | Can replace a dependency within its pin, or the bytes behind a tag/archive |

Explicitly **out of scope** (documented trust boundary): root compromise, kernel compromise,
physical access to the Jetson, and a compromised session-UID process being indistinguishable
from the receiver.

### 2.2 Lanes and coverage

| Lane | Scope | Result |
|---|---|---|
| TLS / transport | `src/tls`, `src/transport/tls_adapter`, `socket_util`, `FrameCodec`, `MessageFramer` | no Critical/High; cipher, timeout and peer-binding gaps |
| Trust store / D-Bus | `src/trust`, `src/ipc/**`, `tests/trust`, `tests/dbus` | fail-closed store; oversize-message DoS; same-UID boundary |
| Parsers / memory safety | `src/replay`, `src/protocol`, framing, session, diagnostics | no OOB/UAF found; one UB overflow in replay path; 2M-case JSON + 200k-case framing fuzz under ASan/UBSan |
| Supply chain / build / host tools | `deps/`, `third_party/`, `cmake/`, `.github/`, `tools/**`, privileged shim | relay trust flaw; self-referential gates; credential history |
| Root helper (main) | `src/helper/**`, packaging, `tools/usb-policy/**` | no auth bypass; connection-delegation limit; bond bookkeeping loss |
| Receiver runtime (main) | `tools/wired-receiver/main.cpp`, `src/app/**`, `src/ipc/**` | documented descriptor-trust limitation; post-TLS plaintext downgrade; same-UID media exposure |

### 2.3 Tools and evidence

- Full host configure/build of both revisions (GCC 13, Ninja, C++20).
- Targeted CTest runs: 150 security-relevant tests on HEAD, 209 on main.
  - HEAD: 148/150 pass. The 2 failures (`tls_posture_reference`, `tls_posture_key_scan`) are
    workspace artifacts: `tools/tls/scan.py` rejects **any symlink** anywhere in the tree, and
    this checkout contains untracked `.codegraph` and `.claude/worktrees/**` symlinks. Both
    tests pass on a clean `git archive` export of HEAD.
  - main: 163/209 pass. All 46 failures trace to absent root provisioning
    (`/etc/android-auto-receiver/{session.uid,receiver.sha256,ap.conf}`), absent
    NetworkManager/BlueZ test buses, or the export having no `.git` history (the key-scan
    regression test re-derives historical blobs). This is itself evidence that the helper
    fails closed when provisioning is missing.
- `clang-tidy` (`clang-analyzer-*`, `bugprone-*`, `cert-*`) across 59 project-owned `src/**`
  translation units: only false positives (optional-invariant blind spots) and one ignored
  `fclose` return.
- Manual code traces for every finding below; adaptive fuzzing performed in the parser lane.
- JSON parser: 2,000,000 mutated inputs; framing/reassembly: ~200,000 random sequences; no
  sanitizer findings. One hand-built UBSan repro for the replay-timestamp overflow.

### 2.4 Limitations

- No real-phone, real-Bluetooth, real-NetworkManager or real-Jetson run. Device-level claims
  in docs were not re-qualified.
- The audit did not review `third_party/aasdk` internals line by line; the TLS patch and the
  effective staged source were reviewed, the rest was treated as a pinned dependency.
- GitHub rulesets, branch protection and the private companion workflow were not inspected
  (outside the repository); the report only evaluates what the tree can enforce.
- No formal proof of the custom D-Bus wire parser; it was fuzzed/read, not verified.

---

## 3. Findings

Severity: **High** = exploitable prerequisite to compromise the project or a host with normal
use; **Medium** = meaningful weakening, DoS, or requires an unlikely-but-plausible precondition;
**Low** = hardening/defense-in-depth; **Info** = observation.

### 3.1 High

#### H1 — The pre-merge ARM64 relay executes unreviewed PR code with owner credentials and the private sysroot

**Files:** `tools/ci/arm64-cross-relay.sh:76-114,147-151`
**Status:** CONFIRMED

The script is the documented way to produce the required `ci/arm64-cross-build` status
(`docs/CONTRIBUTING.md:59-71`). It:

1. checks out the exact PR head SHA into a detached worktree (`:81`);
2. runs, from that worktree, `python3 tools/build/validate_sysroot.py`, `cmake --preset` and
   `cmake --build` (`:111-114`);
3. then posts a `success` commit status as the owner through `gh` (`:147`).

Everything executed in step 2 comes from the contributor's tree: the sysroot validator, CMake
modules, `tools/deps/*.py`, and arbitrary build-time `execute_process`. That process inherits
`$HOME`, including `~/.config/gh` (a token that can write commit statuses on this repository),
the private observed sysroot, and the maintainer's SSH/agent environment. A malicious PR can
therefore (a) forge the acceptance evidence it is being judged by, (b) exfiltrate the token or
sysroot, and (c) get a green required check posted for its own SHA.

**Fix:**
- Run the build in a throwaway VM/container with no `$HOME`, no `gh` auth, a read-only sysroot
  mount, and no network.
- Run the validator from the trusted base checkout, not the PR worktree.
- Post the status from a separate step after the sandbox exits, keyed only to the SHA.
- Restrict the relay to SHAs already approved by a human.
- Scrub the environment (`env -i`, fixed `PATH`, `GIT_CONFIG_NOSYSTEM=1`).

---

### 3.2 Medium

#### M1 — A single oversized or unsupported D-Bus message desynchronizes and terminates/wedges the control service

**Files:** `src/ipc/dbus_adapter/BusConnection.cpp:82-99`, `src/ipc/dbus_adapter/BusMessage.cpp:15-38`, `src/ipc/dbus_adapter/BusWire.hpp:24`, `src/helper/BusService.cpp:205-216`, `src/helper/main.cpp:56-60` (main)
**Status:** CONFIRMED and **reproduced end-to-end through a real `dbus-daemon`** (§7.2)

`read_message()` reads a 16-byte head, then `message_size()` rejects anything above the private
64 KiB limit *before reading the body*. The body stays queued in the socket, so the stream is
now mid-message. Every subsequent read parses garbage and fails. In the helper, any `cycle()`
error calls `bus_lost()` and `main()` returns 1, so the service exits (and is restarted after
2 s). Any process that a policy grants delivery can keep it down; the packaged default policy
denies delivery, but a per-UID grant is required for the product to work, so the attacker set is
"the receiver account" — i.e. any same-UID process. On the session-bus control service the same
desync is reachable by any session-UID process with no authorization at all (for example a
70 KiB method call delivered to the receiver's connection). The parser also accepts only
little-endian messages, so a valid big-endian message has the same effect.

**Root cause (added by validation): a limit mismatch.** `dbus-daemon`'s session configuration
(`/usr/share/dbus-1/session.conf:66`) sets `max_message_size` to **1,000,000,000**, while
`BusWire.hpp:24` caps messages at **64 KiB**. The daemon therefore forwards messages ~15,000×
larger than the client can parse, and nothing reconciles the two. Draining the socket on oversize
is necessary but **not sufficient**: the receiver would still be unable to accept any legitimate
message above 64 KiB. The fix must address both.

**Victim direction (clarified by validation):** the oversize message is what breaks the process
that *owns the `BusConnection`*. The DoS is therefore on the receiver, not on the attacker.

Related parser looseness, lower risk because dbus-daemon/dbus-broker validate signatures
before forwarding: `skip_value("a()")` with a non-zero length loops forever (reproduced with a
harness built from `BusParse.cpp:190-227`), array alignment is derived from the innermost
element type, and duplicate header fields are last-wins (`BusMessage.cpp:96-101`). Validation
confirmed the daemon-level mitigation: sending signature `a()` over a real bus is **rejected by
the daemon**, so the hang is not remotely reachable (§7.3).

**Fix:** (a) reconcile the limits — drain exactly `body_len + fields_len` bytes and discard the
message, *or* close and reconnect on oversize, **and** either lower the shipped bus
`max_message_size` or raise the adapter ceiling deliberately; never leave the stream mid-frame.
(b) Add a rate limit and a regression test that sends 70 KiB and then a valid call. (c) Reject
empty structs, duplicate and unknown header fields, and compute alignment from the immediate
element type.

#### M2 — Pairing confirmation is not reliably bound to an identity the user can see

**Files:** `src/trust/PairingPolicy.cpp:14-16,56-78` (**HEAD**); main narrows this via helper
observation (`src/ipc/ControlPairing.cpp:27-45`) but `PairingRequested` still carries only the
request id (`src/ipc/ControlPairing.cpp:51`)
**Status:** CONFIRMED for HEAD; partially mitigated on main

On HEAD, `candidate_` is overwritten on every discovery and never cleared. `pairing_requested()`
snapshots whatever candidate exists **at request time**, and the request signal carries no
identity. A phone seen earlier (possibly an attacker who then left) is approved when the owner
clicks "Add phone" for their own device. On main the snapshot is narrowed: the candidate is
refreshed from a helper-observed BlueZ peer when the request is created, and the header states
the snapshot-only contract explicitly (`include/aa/trust/PairingPolicy.hpp`). That removes the
stale-candidate window but still binds an identity the user never sees: `PairingRequested` carries
only the request id, so Confirm cannot be a deliberate decision about a specific phone.

**Fix:** clear candidates on disconnect/window close; include a redacted identity or short code
in `PairingRequested` and require `ConfirmPhonePairing` to echo it; enforce a freshness bound.

#### M3 — "Same Unix UID" is the whole authorization boundary for trust and control

**Files:** `include/aa/trust/Store.hpp:18-19,64-65`, `src/trust/StoreFile.cpp:45-61`,
`src/ipc/ControlService.cpp:51-97` (HEAD), `src/helper/Identity.cpp` (main helper, stricter)
**Status:** CONFIRMED by design

The approved-phone store lives in `$XDG_STATE_HOME/android-auto-receiver/approved-phones.json`
with 0600/0700 permissions, and the control service authorizes any caller whose UID equals
`session_unix_uid`. Any same-UID process can therefore edit the store directly, or register as
the consumer (first-come) and approve/revoke phones without a user-presence step. This matches
the documented "receiver is the trust anchor" model, but it should be an explicit deployment
decision, not an implicit one.

**Fix:** run the receiver with a dedicated service UID/DynamicUser and keep the HMI in a
separate UID; or add a receiver-side confirmation that cannot be satisfied by a background
process (polkit, seat/active-session check, hardware button). Document "same UID = trusted".

#### M4 — Bluetooth bond revocation cannot work after a helper restart

**Files:** `src/helper/bluez_adapter/Platform.hpp:39-40` (`std::vector<PeerIdentity> managed`),
`src/helper/bluez_adapter/Peers.cpp:90-93,132-148`, `docs/system-helper.md:175-181` (main)
**Status:** CONFIRMED

`managed` is an in-memory allow-list populated when the helper observes/authorizes a device.
`revoke_peer()` rejects any identity not in it. BlueZ bonds persist across helper restarts, so a
phone bonded before the restart can no longer be removed through `RevokePeerV1`, while the bond
itself remains in BlueZ. The trust store still blocks normal reconnection through this helper,
so this is not automatic reauthorization, but it is an incomplete revocation and a known
production blocker per the docs.

**Fix:** persist a durable, authenticated managed-bond set (or a revocation journal) and
reconcile it at startup; report revocation as complete only after `RemoveDevice` succeeds.

#### M5 — CI gates and provenance anchors are self-referential and PR-modifiable

**Files:** `.github/workflows/ci.yml:9-23,126,198`,
`tools/deps/check_manifest.py:40-67`, `deps/manifest.lock`, `tools/provenance/inventory.py:42-57`
**Status:** CONFIRMED

A pull request runs its own copy of the workflow, the manifest checker, the lock file, the
provenance inventory and the scenario tests. One PR can change a constant and the data it
checks together and stay green. There is no `CODEOWNERS`, no dependency bot, and the public
`ci/arm64-cross-build` job uses the same status context as the private relay while always
reporting fixture mode — so a branch protection that requires the context name cannot tell
fixture from observed acceptance. Anyone with commit-status write access can also post that
context.

**Fix:** add `CODEOWNERS` for `/.github/`, `/deps/`, `/tools/deps/`, `/tools/provenance/`,
`/tools/tls/`, `/toolchains/`, `/third_party/provenance/` and enforce owner review in the
ruleset; run validators from the base branch (`git show origin/main:...`) on PRs; rename the
fixture job (`ci/arm64-cross-fixture`) and bind the observed check to a specific App
(`integration_id`); sign the observed manifest (cosign/minisign) and verify against a pinned key.

#### M6 — The historical third-party head-unit private key is still in Git history and in 23 branch tips

**Paths:** `third_party/aasdk/cert/headunit.key`, `third_party/compat-credentials/headunit.key`,
embedded literals in old `Cryptor.cpp`; commits `ab3a7f6`, `8064a2f`, `1429ebb`, `b37114c`,
`aa98635`, `952feb8`; 23 `origin/aa/*` tips still contain the file
**Status:** CONFIRMED (`git cat-file -e` against branch tips; key is the public JVC-Kenwood
compatibility credential, not a production secret of this project)

The current trees (HEAD and main) are clean, and `tools/tls/scan.py` plus the provenance checks
enforce that. But `SECURITY.md` says "no production private keys", while the key remains
readable in history and in 23 remote branch tips. It is a publicly known upstream credential and
phones do not use it as a trust anchor, so the direct exploitability is low; the issue is
release/compliance hygiene and the stale branches that would re-introduce it if merged.

**Fix:** delete/reset the 23 stale `aa/*` branches; add a "historical exposure" note to
`SECURITY.md`; decide before any public tag whether history must be rewritten; keep the scan
gate as the merge backstop. Also remove the ~29 stale `headunit.*` copies (mode 0664) left under
`.omo/worktrees/*` by old branches.

#### M7 — Production wireless cipher policy admits static-RSA suites and ignores setup failures

**Files:** `src/transport/tls_adapter/TlsRecordCodec.cpp:58-63` (main; also HEAD),
`src/tls/Random`-independent `SSL_CTX_set_cipher_list` return unchecked
**Status:** CONFIRMED

The AAP record codec (used by `make_client_handshake_cipher`, the real wireless handshake)
allows `AES128-GCM-SHA256:AES256-GCM-SHA384` in addition to ECDHE suites. The static-RSA
variants provide no forward secrecy: a recorded session plus a future HU-key compromise decrypts
traffic. The return value of `SSL_CTX_set_cipher_list` is ignored, so a failure silently falls
back to OpenSSL defaults (and the later `strstr(name,"GCM")` check only verifies the name).

**Fix:** check every SSL_CTX setter return; prefer ECDHE-only if phone interop allows (verify
against the target phone models); if static RSA is required for compatibility, document it and
rotate the HU credential on a schedule.

#### M8 — Target binaries are non-PIE with no compiler/linker hardening

**Files:** `toolchains/JetsonRuntime.cmake:52` (`-no-pie` + `crt1.o`), `CMakeLists.txt`/`cmake/*.cmake`
(no `-fstack-protector-strong`, `-D_FORTIFY_SOURCE`, `-Wl,-z,relro,-z,now`)
**Status:** CONFIRMED

Jetson executables (including the future root helper) lose ASLR for their own image and gain no
stack-protector or RELRO/BIND_NOW. This is a defense-in-depth loss for a network/radio-facing
daemon.

**Fix:** build PIE (`-fPIE -pie` with the sysroot `Scrt1.o`), add
`-fstack-protector-strong -D_FORTIFY_SOURCE=2 -Wl,-z,relro,-z,now`, and assert the result in
`tools/build/contamination.py` via `readelf` (Type DYN, GNU_RELRO, BIND_NOW).

#### M9 — Plaintext AAP frames are accepted after the TLS handshake completes

**Files:** `src/transport/MessageFramer.cpp:104-126`, `src/app/WiredSession.cpp:374-379` (main;
the same reassembler is used by the replay player on HEAD)
**Status:** CONFIRMED (runtime path verified; re-confirmed by independent code trace in §7)

`MessageReassembler::push()` calls `cipher_->open()` only when the frame's ENCRYPTED flag is set;
otherwise it copies the payload through. The resulting `Message` does not carry the flag, so
`WiredSession::route()` cannot tell a post-handshake plaintext message from an encrypted one: it
only checks `authenticated()`. An attacker positioned on the USB link (or a malicious admitted
phone) can therefore inject plaintext control/service frames after the legitimate TLS handshake,
bypassing TLS record integrity for those frames. The prerequisite is the same physical/on-path
position that the wired trust model already treats as physical-access trust
(`include/aa/trust/Identity.hpp:12-14`), which caps practical impact, but the downgrade is cheap
to close and currently breaks the "TLS protects the session" claim.

**Fix:** reject ENCRYPTED=0 frames on all channels once the cipher is active, except an explicit,
tested compatibility allow-list; or carry the encryption flag in `Message` and enforce the
policy in the router.

#### M10 — Pairing approval is committed before the helper confirms it

**Files:** `src/ipc/ControlPairing.cpp:59-80` (main)
**Status:** CONFIRMED (code comment states the behavior)

`confirm_phone_pairing()` calls `phones_.pairing_confirmed()` (which persists the approval)
before `confirmation_->confirmed()`. If the helper call fails, the gate is cancelled but the
approval is deliberately **not** rolled back, so the UI reports a failed pairing while the phone
remains trusted for later admission. This component is not active in the wired entry point's
rejecting pairing callback today, but it is the path the wireless flow uses.

**Fix:** make approval transactional (stage, then commit after helper success), or persist an
explicit "approved-but-unconfirmed" state that admission rejects until confirmed.
#### M11 — An intermediate symlink component redirects the trust store outside the state tree

**Files:** `src/trust/StoreFile.cpp:90-118` (`create_missing_ancestors`), `:214-283`
(`write_file_private`), `:163-206` (`read_file`). Present on **HEAD and main**.
**Status:** REPRODUCED (2 failing regression tests added)

`StoreFile.hpp` documents that "reads and writes traverse ONLY the verified immediate parent
descriptor — a symlinked parent is a rejection that mutates nothing (no path-based chmod
anywhere)". That holds for the **immediate** parent, and the suite proves it
(`StorePermissions.SymlinkedParentWriteFailsClosedWithoutMutatingTarget`).

It does **not** hold for an intermediate component. `create_missing_ancestors()` walks the path
with `lstat`/`mkdir`/`open` **by path**, so a symlink at any component above the store directory
is followed. Because `<component>/android-auto-receiver/` does not exist yet, it is created
*through* the symlink and the store lands in the symlink target:

```
state -> /elsewhere                              (intermediate symlink)
<state>/android-auto-receiver/approved-phones.json
   -> /elsewhere/android-auto-receiver/approved-phones.json
```

Reproduced in both directions:

- **Write:** `write_file_private` returns SUCCESS and creates the directory and file outside the
  state tree.
- **Read:** a private `0600` store planted in the symlink target is **loaded successfully**, and
  its phones report `Decision::approved`.

**Impact.** The store is the authorisation record for which phones may connect. A store read from
a directory the receiver did not intend to trust is an authorisation input chosen by whoever
controls that directory, and the symlink-safety the header promises is silently defeated.

**Precondition.** Control of `$XDG_STATE_HOME` or a component of the state path. On a desktop that
is the same UID, which the design already trusts, so this is not a privilege boundary crossing by
itself. It becomes material on the provisioned Jetson appliance, where the state path is
machine-managed and the assumption "the receiver alone decides where its store lives" no longer
holds. Related but distinct from L10 (key-scan symlink false positive) and L16 (predictable temp
paths in tools).

**Fix:** make ancestor creation fd-anchored — `openat(parent_fd, component,
O_DIRECTORY|O_NOFOLLOW)` plus `mkdirat`, descending one verified descriptor at a time and rejecting
any non-directory or symlink component. Never `mkdir` by absolute path.

**Regression tests:** `StorePermissions.IntermediateSymlinkComponentCannotRedirectTheWrite` and
`StorePermissions.IntermediateSymlinkComponentCannotLoadAnExteriorStore`.

#### M12 — A serial-less wired approval aliases every serial-less device on that port

**Files:** `src/trust/Identity.cpp:33-39` (`wired_has_material`), `:72-88` (`validated_key`).
Present on **HEAD and main**.
**Status:** REPRODUCED (1 failing regression test added)

`include/aa/trust/Identity.hpp` states the store keeps "the STRONGEST available transport identity
per phone". `wired_has_material()` returns true when there is **no serial at all**, as long as
VID, PID and `port_context` are present:

```cpp
return !wired.usb_vendor_id.empty() && !wired.usb_product_id.empty() &&
       !wired.port_context.empty();
```

A device reporting no USB serial and no AOA serial (common for bare/serial-less implementations)
is therefore approved on a key like `w1:4:18d14:4ee10:0:0:0:5:1-2.3`. Two **different physical
phones** in that state produce byte-identical keys, so approving phone A reports
`Decision::approved` for phone B with no further user action.

**Relationship to the documented trust model.** The project explicitly accepts that wired identity
is physical-access trust and that descriptors can be spoofed by someone holding the port. That
covers "an attacker who plugs in a spoofing device". It does not cover the weaker case this
introduces: the owner approves *their own* serial-less phone and an **unrelated** serial-less
device becomes approved, because the strongest-identity claim is not enforced when the strongest
fields are absent. Severity is Medium rather than High only because it stays inside the documented
physical-access boundary.

**Fix (either):** refuse a wired identity with no serial (fail closed, forcing a stronger
identity), or record the approval at an explicitly weaker tier and surface that in
`redacted_summary()` and the pairing UI so a port-only grant is a visible, deliberate decision.

**Regression test:** `TrustStore.SerialLessApprovalDoesNotAliasADifferentDevice`.



---

### 3.3 Low

| ID | Finding | Evidence | Fix |
|---|---|---|---|
| L1 | **No read/idle deadline after TLS handshake**; blocking `connect`/`accept` in `TlsTransport`/`TlsListener`. A silent peer can pin the session thread. Production wired runtime uses a deadline-bounded link and `poll_receive` with a deadline, so exposure is limited to the generic TLS adapter (test/loopback-only). | `TlsStream.cpp:52-53,80-81`; `socket_util.hpp:32-69,115-138,189-203`; main `src/app/WiredSession.cpp:330-345` | Add per-frame/idle deadline and cancellation to all transport reads; `SO_KEEPALIVE`/`TCP_USER_TIMEOUT`; non-blocking connect with poll token. |
| L2 | **TLS admission is not bound to the TCP peer.** `accept()` discards the peer sockaddr and the approved predicate is parameterless; in `encryption_only_compatibility` `SSL_VERIFY_NONE` is used by design. Latent on `TlsListener`; production wireless binds admission to the coordinator-selected Bluetooth phone. | `TlsListener.cpp:20,54`, `Policy.cpp:24-31`, `socket_util.hpp:194`, main `src/app/WirelessLink.cpp:40-70` | Pass `PeerInfo` (sockaddr + optional cert fingerprint) to the predicate; require it to match the expected phone/AP client. |
| L3 | **`verified_peer_test_only` does not actually require a peer cert.** Server role sets `SSL_VERIFY_PEER` without `SSL_VERIFY_FAIL_IF_NO_PEER_CERT`; no trust store is loaded and no hostname check is done. Test-only enum value exposed in a public header. | `Policy.cpp:29-31`, `TlsTransport.cpp:26-45` | Add `FAIL_IF_NO_PEER_CERT` + a real pin/trust path, or remove the enum value. |
| L4 | **Signed-overflow UB in replay deadlines** from a fixture timestamp near `INT64_MAX`; UBSan-reproduced, leads to a negative deadline and a spurious failure transition. Fixture loader bounds relative span, not absolute `t`. | `src/session/Lifecycle.cpp:73-79`, `src/replay/FixtureValidate.cpp` | Cap absolute fixture time; use saturating addition in `arm_deadline`. |
| L5 | **Revocation durability and canonicalization gaps (HEAD).** `forget_phone` rolls back in memory when persistence fails (revoked phone stays approved); no directory fsync after rename, so power loss can resurrect a revoked phone; an active session is not stopped on revoke. Wireless bond identities are compared bytewise, so a differently-cased MAC is a distinct key and only exact-key aliases are removed by `forget`. Main fixes the durability and session-stop gaps (durable-commit semantics + `WiredPhoneControl::checkpoint`). | `src/trust/Store.cpp:170-181`; `src/trust/StoreFile.cpp:276-284` (HEAD); `src/trust/Identity.cpp:62-91`; main `StoreFile.cpp:300-330`, `src/app/WiredPhoneControl.cpp` | Backport main's semantics if HEAD is still deployed; canonicalize wireless bond addresses (uppercase, validate `AA:BB:..`) in `validated_key`. |
| L6 | **Credential loader accepts expired/undersized keys** and an unencrypted key by design. It thoroughly checks ownership, mode (exactly 0600), link count, symlink and cert/key match. | `src/tls/Credentials.cpp:44-78` | Reject `notAfter` in the past, key bits < 2048, weak signature algorithms; document plaintext-at-rest. |
| L7 | **USB policy rollback does not revoke existing live ACLs.** It restores rules but `uaccess` ACLs already applied remain until re-enumeration/session end; the README documents this. | `tools/usb-policy/rollback.sh:52-74,174-185`, `70-android-auto-receiver.rules:60-63` (main) | Report "rules restored" vs "access revoked" distinctly; reconcile node ACLs if immediate revocation is required. |
| L8 | **`eval` of an environment variable in root USB scripts.** Only in `AA_USB_POLICY_ROOT` test mode and requires a sudo rule that preserves/permits the env; ordinary `sudo` resets it. | `tools/usb-policy/common.sh:27-39`, `install.sh:42-47`, `rollback.sh:39-43` (main) | Remove fault injection from shipped scripts; reject `AA_USB_POLICY_*` when euid is 0. |
| L9 | **Root helper unit lacks capability/syscall bounds and core-dump suppression.** It already sets `NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`, `RestrictAddressFamilies`, `RestrictSUIDSGID`, `UMask=0077`. PSK copies in intermediate `std::string`/GVariant are not all wiped. | `packaging/systemd/aa-system-helper.service:7-25`, `src/helper/nm/Files.cpp:89-103`, `nm/Connection.cpp:156-187` (main) | Add `CapabilityBoundingSet`, `ProtectKernel*`, `RestrictNamespaces`, `MemoryDenyWriteExecute` where compatible; `LimitCORE=0`; minimize secret copies. |
| L10 | **Key-scan false positive on any symlink.** `tools/tls/scan.py` rejects every symlink in the tree and prints `credential-material-present`, so any developer checkout with a worktree/symlink link fails the security gate. Fails closed, but trains people to ignore red tests. | `tools/tls/scan.py` (symlink rule); reproduced with one `ln -s` in a clean export | Exclude known non-source paths (`.claude`, build dirs) or report a distinct `symlink-present` reason. Also note `.claude/worktrees/` is only ignored via `.git/info/exclude`, not `.gitignore`. |
| L11 | **Video socket DoS by an authorized same-UID consumer.** `max_datagram_payload` accepts 1 byte, so a 4 MiB access unit becomes ~4M datagrams; the limits handshake can block the accept loop for 60 s per peer. Parent dir is 0700 and peer UID is checked (`SO_PEERCRED`), so cross-user exposure is not present. | main `src/ipc/video/VideoCodec.cpp:135-157`, `VideoChannel.cpp:358-379,914` | Enforce a sensible minimum datagram payload (e.g. 1 KiB); set `umask(0177)` around `bind`; non-blocking handshake. |
| L12 | **Neither side authenticates the bus itself, and the receiver's helper client does not verify the helper owner's UID.** The helper client pins the first unique name owner and checks replies against it, but never asks the bus for that owner's UID; the receiver's control bus connection likewise trusts `DBUS_SESSION_BUS_ADDRESS` without `SO_PEERCRED` or a GUID check. A correctly installed system-bus policy denies helper ownership to non-root and a session bus is same-UID by construction, so this is fail-closed *when correctly installed*; on a private/misconfigured bus a fake bus or helper can feed fabricated AP/peer state. | main `src/helper/BusClientImpl.hpp:23-39`, `src/helper/BusClient.cpp:38-45`, `src/ipc/dbus_adapter/BusConnection.cpp:119-150`, `packaging/dbus/org.custom.AndroidAutoSystemHelper.conf:6-12` | In system mode, call `GetConnectionUnixUser` on the resolved owner and require root; check `SO_PEERCRED` on the bus socket and pin the address in the service unit; keep private-bus fixtures test-only. |
| L13 | **Phone-driven logging can consume disk or block shutdown** (SUSPECTED). Audio completions and periodic video events log continuously; the in-memory queue is bounded but producers wait for space and the writer uses blocking stdout writes with no shutdown deadline. An admitted hostile phone can sustain media traffic; impact depends on the external log sink. | main `tools/wired-receiver/main.cpp:1456-1460`, `VideoOutlet.hpp:81-85,115-123`, `StdoutLog.hpp:34-56,130-151` | Aggregate/rate-limit media telemetry, require bounded log retention, make the sink cancellation-aware with a bounded flush deadline. |
| L14 | **Unknown protobuf fields are handled inconsistently.** Generic parsing checks size, parse success and required fields but does not reject unknown fields; only some adapters apply the strict policy. No corruption or bypass shown, but it creates protocol ambiguity. | `src/protocol/aasdk_adapter/Framing.hpp:42-57`, `ControlAdapter.cpp:88-107`, `UnknownFields.hpp:89` | Define a per-message extension policy and apply it after every parse, including explicit rejection for security-relevant messages. |
| L15 | **The HU credential PEM is copied through ordinary process memory.** Loading reads the whole file into `std::string`/PEM buffers without explicit cleansing, and the receiver service sets no core-dump restriction. A same-UID debugger or readable core dump can recover the key. | `src/tls/Credentials.cpp:48-107`; main `tools/wired-receiver` service packaging | Cleanse temporary PEM buffers on success and error, minimize copies, set `LimitCORE=0`/dump suppression for the service. |
| L16 | **Probe/QA tooling retains device-identifying data and uses predictable temp paths.** `report.raw` keeps `station dump` client MACs, `nm` and BlueZ introspection; a wired-QA script writes `/tmp/task-30-live-qa.json`. Operator-local, not shipped, but leaks identifiers and permits symlink clobber. | main `tools/target/wireless_target.py:121-127,171,180-185`, `tools/wired-qa/run-wired-live-qa.sh:33,82-84` | Redact/hash MACs before serializing; write to `mktemp -d` under `$XDG_RUNTIME_DIR` with 0700. |

### 3.4 Informational

- **I1 — Strong controls verified.** See §4.
- **I2 — Wireless projection fails closed in production** because admission requires
  `AA_WIRELESS_TEST_ATTESTATION` (`src/app/WirelessLink.cpp:100-105`), which only tests define.
  This is safe but means wireless cannot work in a release build; confirm it is intentional.
- **I3 — `config::validate()` does not enforce its own contract**: jurisdiction is only checked
  non-empty (not a 2-letter country), and `microphone_enabled`/`sensors_enabled` are not forced
  false. The API has no callers on main, so this is latent.
- **I4 — Replay fixture SHA-256 is self-attesting** (an attacker can regenerate it). Parsing is
  bounded, so this is not a parser risk; do not treat the hash as provenance.
- **I5 — Wired identity is descriptor-derived and therefore spoofable by an attacker with
  physical USB access; this is documented as accepted physical-access trust, not a defect.**
  `include/aa/trust/Identity.hpp:12-14`, `Store.hpp`, `PairingPolicy.hpp` state that a wired
  identity is `VID/PID/port` (+ serials when present) and that physical port access is the trust
  boundary; TLS in `encryption_only_compatibility` deliberately does not add peer-certificate
  authentication because real phones do not present one. A device that can emulate the approved
  descriptor tuple at the approved port is the approved phone by definition. Keep it as an
  explicit deployment limitation; only a protocol-level device credential could change it.
- **I6 — Helper replay history is bounded per process lifetime.** At most 4,096 issued grants
  are retained, after which further grants are refused until restart
  (`src/helper/SystemHelper.cpp:39-56,141-150`). Fail-closed and bounded, but an authenticated
  receiver can exhaust availability; documented behavior, not a bypass.
- **I7 — Doc drift:** `third_party/aasdk/cert/README.md` still describes an embedded certificate
  fallback and 0644 install that no longer exist; the shim README and `.service` comments
  disagree about installation state; the shim README misstates its own line count. Misleading
  docs can resurrect an insecure install.
- **I8 — Helper authentication is per-connection, not per-writer, and not atomic with the
  effect.** The helper resolves the bus UID/PID and hashes `/proc/PID/exe`, which identifies the
  *connection owner*, not which process is currently writing to an inherited or `SCM_RIGHTS`-
  transferred socket; a deliberately delegated receiver FD would carry the receiver's full
  helper authority. `Execute` re-authenticates immediately before dispatch, while `Invalidate`,
  `RevokeAP1`, `ReconnectPeerV1`, `RevokePeerV1` and `AuthorizeBootstrapV1` authenticate once and
  then act across potentially blocking work. Same-UID process compromise and intentional FD
  delegation are outside the documented trust boundary, so this is recorded as a design property
  rather than a finding; a dedicated non-delegated broker would be required to close it.

---

## 4. Verified controls (what is actually good)

These were checked and found correct; they are the reason there is no Critical/High code finding.

**TLS and transport**
- TLS 1.2 min **and** max pinned, return values checked on the generic transport; record codec
  pins 1.2 and verifies the negotiated cipher contains GCM.
- Frame parsing is bounded end-to-end: reserved flags rejected, plain payload ≤ 16,384,
  encrypted ≤ 16,413, FIRST total ≤ 4 MiB checked before buffering, exact size match, 8 MiB
  queue cap, no unaligned casts, big-endian assembly by explicit shifts.
- TLS record codec validates content type (23), version (0x0303), fragment length, minimum
  fragment, and exact consumption; AEAD failure deactivates the codec (fail closed).
- `SSL_read`/`SSL_write` handle WANT_READ/WANT_WRITE, zero-return as EOF, partial writes, and
  `MSG_NOSIGNAL`; no SIGPIPE.
- Credential loading: `HU_KEY_PATH` only, `O_NOFOLLOW|O_CLOEXEC`, `fstat` on the fd, regular
  file, owner == euid, mode exactly 0600, `nlink == 1`, ≤ 64 KiB, encrypted keys refused, cert/key
  match, trailing garbage rejected, no path in error text.
- TLS key scan enforces zero credential copies in the tracked tree, including base64/hex/DER
  re-encodings bound to the provenance record.

**Trust store**
- Fail closed on missing/corrupt/wrong-owner/wrong-mode/symlink/oversize/unknown-version/
  duplicate-id/duplicate-identity stores; `usable_` gating; lookups read memory only.
- Atomic replace: verified parent dir fd, `O_EXCL` random temp, `fchmod 0600`, `fsync`, `renameat`,
  and on main a directory `fsync` with explicit `trust_store_not_durable` semantics.
- Identity keys are length-prefixed and injective; exact comparison; printable ASCII ≤ 512;
  revocation removes every alias; pairing pending entries are capped (8) and expire (60 s).
- HEAD's JSON parser rejects `\u`, floats, negatives, booleans, null, duplicates, leading zeros
  (in the trust parser) and trailing garbage; depth 8, 512-byte strings, 256 KiB document.

**D-Bus / IPC**
- Caller identity comes only from the daemon-stamped unique sender; `GetConnectionUnixUser`;
  `GetNameOwner` must equal the registered connection; replies require serial + sender +
  destination (no reply spoofing).
- `AUTH EXTERNAL`; no fd passing negotiated; messages ≤ 64 KiB with ≤ 16 header fields and
  exact body-length consistency.
- Method table is exact on path/interface/member/signature; `Properties.Set` is read-only;
  unknown calls rejected; mutating methods require the consumer binding.
- Video/input AF_UNIX sockets: parent dir 0700/verified, node 0600, `SO_PEERCRED` checked on both
  ends with `expected_uid` set to `current_uid()` by the runtime, `SOCK_CLOEXEC`, bounded
  records, sequence/order validation, drop-to-IDR recovery.

**Root helper (main)**
- Provisioning is anchored, component-by-component: root-owned non-writable path components,
  no symlinks, strict decimal UID, 64 lowercase hex digest, ≤ 128-byte files; startup aborts on
  any deviation.
- Every helper method calls `authenticate()`, which resolves the bus UID+PID, inspects
  `/proc/PID` (four UIDs, live state, starttime), hashes the opened executable, and pins the
  tuple to a single receiver; the accumulated identity is re-resolved on every tick.
- Privileged actions need a random one-use grant bound to source/action/channel, with replay
  history; `Execute` re-authenticates immediately before dispatch.
- Command execution uses fixed absolute `iw`/`nft` with argv vectors and a minimal environment;
  no shell. PSK generated with `RAND_bytes` (256-bit), persisted 0600 in a verified 0700
  directory via `O_NOFOLLOW` + rename + fsync.
- The packaged system-bus policy denies owning and sending to the helper by default; only root
  may own it, so a missing per-UID grant fails closed.

**Parsers / diagnostics**
- 2M mutated JSON inputs and ~200k framing sequences: no ASan/UBSan findings.
- Diagnostics use a closed event vocabulary and key whitelist; untrusted values are typed,
  UTF-8-sanitized and JSON-escaped; no printf-family use with untrusted format strings in `src/`.
- `SessionThread` is the single owner of session state; posted tasks never run concurrently;
  cancellation is a C++20 stop token.

**CI**
- Only `actions/checkout` is used, pinned to a full SHA; `permissions: contents: read`;
  `persist-credentials: false`; no `pull_request_target`, `workflow_run`, secrets, artifacts or
  cache; the only `curl` uses `--proto '=https' --proto-redir '=https'` followed by SHA-256
  verification; `apt` only via fixed package names.
- No network access during CMake configure (`FETCHCONTENT_SOURCE_DIR_GOOGLETEST` guard,
  no `file(DOWNLOAD)`/`ExternalProject`), and the archive is verified against
  `deps/manifest.lock`.
- Mutation-tested `check_manifest.py`: patch change, commit change, lock change, bad archive
  and missing lock all fail closed.

---

## 5. Recommended order of work

1. **Fix the trust-store symlink walk (M11).** Small, contained, fixes both the read and write
   paths, and it is the only finding where a *store the receiver does not control* can be loaded.
2. **Fix the relay (H1).** This is the only finding that can compromise the project itself
   through normal use. Sandbox the build and separate status posting.
3. **Fix D-Bus oversize handling (M1).** Reconcile the 64 KiB adapter ceiling with the bus
   `max_message_size` **and** drain/close on oversize; removes a trivially-triggered kill/wedge of
   the helper and session control.
4. **Stop aliasing serial-less phones (M12).** Refuse a wired identity with no serial, or make
   port-only approval an explicit, visible tier.
5. **Reject plaintext frames once TLS is active (M9).** Also small and local, and it removes the
   gap between the documented "TLS protects the session" claim and what the router enforces.
   Carry the encryption flag through `Message` so the policy cannot silently regress.
6. **Decide and document the same-UID boundary (M3)**, give pairing a user-visible identity
   (M2), and make approval transactional with the helper (M10). These are the questions a
   security review of the product will ask first.
7. **Make CI trust non-self-referential (M5).** Add CODEOWNERS, base-branch validators, a
   distinct fixture context, and a bound observed check.
8. **Close the Bluetooth revocation gap (M4)** with durable managed-bond reconciliation, and
   verify the helper owner's UID in the receiver's client (L12).
9. **Harden target builds (M8)** and the helper unit (L9), and enforce a sane minimum datagram
   size on the video socket (L11); all three are cheap and measurable.
10. **Backport or retire the HEAD-only gaps** (L1-L3, L5) if that branch is still shipped;
    otherwise record them as historical. L4 and L6 exist on both revisions.
11. **Hygiene (M6, L10, L14-L16)** — stale branches with the old key, `.gitignore` gaps,
    misleading docs, log-sink bounds, probe-data redaction.

Suggested acceptance evidence for the fixes: a regression test for oversized D-Bus messages
followed by a valid call; a test that a post-handshake plaintext frame is rejected; a unit test
that a stale pairing candidate cannot be confirmed; a helper-failure test proving no approval
persists; a CI job that shows a PR cannot pass with a modified `check_manifest.py`; `readelf`
assertions for PIE/RELRO; and a relay test that runs with an empty `$HOME` and no `gh` config.
For M11 and M12 the tests already exist (§7.4) — they fail today and should land first, so each
fix is provably targeted.

---

## 6. Where the two passes disagree

The independent validation pass (§7) agreed with the severity of every original finding, but
strengthened M1 and added M11/M12. The points worth recording:

1. **M1 is stronger than first reported.** The root cause is not only a parser that fails to
   drain; the client adopts a 64 KiB ceiling against a bus configured for 1 GB. Draining alone
   still leaves the receiver unable to accept legitimate messages over 64 KiB.
2. **The `a()` infinite loop is correctly a low-severity note**, not a remote finding: the daemon
   rejects the signature, so it is unreachable from the wire. The evidence is now a live daemon
   test rather than a code reading.
3. **The original pass missed M11 and M12**, both in `src/trust` — the most security-relevant
   module — and both contradicting their own header documentation. Neither was covered by an
   existing test, and the existing symlink tests create a false sense of coverage because they
   only exercise the *immediate* parent.
4. **L10 is confirmed as a workspace artifact**, not a regression: the key scan fails only because
   this checkout has untracked symlinks (`.codegraph`), and passes on a clean `git archive`.

---

## 7. Independent validation pass

A second pass, run by a single agent with no subagents, re-verified the findings by building and
*executing* the production code rather than reading it. Conclusion: the audit is
**substantially accurate**, and the headline findings acquire stronger evidence.

### 7.1 Method

Standalone reproducers were compiled against the real project sources (GCC 13.3, C++20, ASan+UBSan
on targeted cases) and run against a real `dbus-daemon` 1.14.10. Reproducers live outside the
repository in `/tmp/validate/` so the audit stays read-only.

### 7.2 M1 reproduced end-to-end

- **Unit level** (`bus_desync_test.cpp`): a 128 KiB message on a socketpair, fed to the real
  `BusConnection::read_message()`. The first read is rejected as oversize and **the next valid
  16-byte message can no longer be read**.
- **End-to-end** (`bus_e2e_test.cpp`, under `dbus-run-session`): an attacker connection owning a
  well-known name sends a **70 KiB** method call to a victim connection.

  ```
  [1] attacker sent a 70 KiB call through the daemon
  [2] victim's read of the oversize message FAILED
  [3] victim CANNOT read after the oversize message
  RESULT: M1 REPRODUCED end-to-end through a real bus
  ```

### 7.3 Findings re-checked by execution or trace

| Finding | Check performed | Result |
|---|---|---|
| M1 | Live unit + real-bus repro | Confirmed end-to-end; root cause amended (limit mismatch) |
| H1 | Traced `run_step` (`cd "$worktree"`) and the `gh api .../statuses/$head_sha` tail | Confirmed as described |
| M9 | Traced `MessageFramer.cpp:104-119` — the `plain` branch never consults `cipher_` | Confirmed |
| M2 | Traced `PairingPolicy::note_candidate` / `pairing_requested` | Confirmed; `candidate_` is never cleared |
| `a()` loop | Live daemon test (`skip_e2e.cpp`) sending signature `a()` | Loop is real but **daemon-rejected**: unreachable |
| L10 | `scan.py` on the tree vs. a clean `git archive HEAD` | Confirmed as a workspace artifact |
| Test counts | `ctest -R 'trust_\|transport_\|session_\|dbus_contract\|tls_'` | 98% / 2 failures, matching Appendix B |

### 7.4 Evidence added to the repository

Three regression tests were added to `tests/trust/`; they **fail today**, each reproducing a real
defect, and all 79 pre-existing trust tests still pass:

| Test | Finding |
|---|---|
| `StorePermissions.IntermediateSymlinkComponentCannotRedirectTheWrite` | M11 (write path) |
| `StorePermissions.IntermediateSymlinkComponentCannotLoadAnExteriorStore` | M11 (read path) |
| `TrustStore.SerialLessApprovalDoesNotAliasADifferentDevice` | M12 |

Verified results after adding them:

```
$ ./aa_trust_store_test --gtest_filter='StorePermissions.*:TrustStore.*'
38 tests ran, 35 passed, 3 failed   (exactly the three tests above)

$ ./aa_trust_store_test --gtest_filter='-StorePermissions.Intermediate*:TrustStore.SerialLess*'
79 tests ran, 79 passed             # no pre-existing test regressed
```

No production source was changed: `git diff --stat -- src/ include/ cmake/ tools/ .github/` is
empty.

### 7.5 Reproducers (not committed)

| File | Demonstrates |
|---|---|
| `bus_desync_test.cpp` | M1 at the unit level (real `read_message`) |
| `bus_e2e_test.cpp` | M1 through a real `dbus-daemon` |
| `skip_hang.cpp` | `a()` infinite loop in `skip_value` (hand-built buffer) |
| `skip_e2e.cpp` | Daemon rejects `a()`, so the loop is unreachable |
| `ancestor.cpp` | M11 on the write path |
| `read_follow.cpp` | M11 on the read path |
| `identity_alias.cpp` | M12 key collision |

---


---

## Appendix A — Audited file inventory (primary)

`src/tls/{Credentials,Policy}.cpp`, `src/transport/**`, `src/transport/tls_adapter/**`,
`src/trust/**`, `src/ipc/**` (including `dbus_adapter`), `src/replay/**`, `src/protocol/**`,
`src/session/**`, `src/channels/**`, `src/core/**`, `src/config/**`, `src/diagnostics/**`,
`include/aa/**`, `src/helper/**` + `packaging/**` + `tools/usb-policy/**` (main),
`tools/wired-receiver/**` + `src/app/**` + `src/ipc/{video,input}/**` (main),
`deps/**`, `cmake/**`, `CMakeLists.txt`, `CMakePresets.json`, `toolchains/**`,
`.github/workflows/ci.yml`, `tools/{ci,deps,provenance,sysroot,build,target,tls}/**`,
`tools/jetson/rtl8822ce-regd-shim/**`, `tests/{tls,trust,dbus,ipc,transport,replay,session,helper,provenance}/**`.

## Appendix B — Test evidence

| Run | Result |
|---|---|
| HEAD `build/audit`, 150 security-relevant CTest cases | 148 pass; 2 fail only because the workspace has untracked symlinks (`scan.py` false positive); both pass on a clean `git archive` export |
| HEAD clean export `tools/tls/scan.py` | PASS zero-credential-copies |
| HEAD clean export `tests/tls/scan.py` / `reference.py` | PASS |
| main `build/audit`, 209 security-relevant CTest cases | 163 pass; 46 fail on missing root provisioning / NM+BlueZ test buses / missing `.git` history for the historical-blob regression |
| `clang-tidy` 59 TUs | no true positives |
| Fuzzing (parser lane) | 2M JSON, 200k framing — no sanitizer reports |
| UBSan repro | `Lifecycle::arm_deadline` signed overflow (L4) |
| Validation pass, `ctest -R 'trust_\|transport_\|session_\|dbus_contract\|tls_'` | 98% pass, 2 fail — the same two symlink false positives (§7.3) |
| Validation pass, `aa_trust_store_test` with the 3 new tests | 38 ran, 35 pass, 3 fail (the new M11/M12 regressions); 79 pre-existing pass |

## Appendix C — Out of scope

- GitHub rulesets, branch protection, the private companion workflow and the actual `gh` token
  scope (not visible from the repository).
- `third_party/aasdk` beyond the TLS patch and the effective staged source.
- Kernel, firmware, bootloader and Jetson BSP hardening.
- Real-device qualification (phone, BlueZ, NetworkManager, Wi-Fi AP, USB).
- The operator's real credential under `~/.local/state/...` (outside the repo; not read).

## Appendix D — Reproducer inventory (validation pass)

All in `/tmp/validate/`, compiled against the real sources; not committed.

| File | Demonstrates |
|---|---|
| `bus_desync_test.cpp` | M1 at the unit level (real `read_message`) |
| `bus_e2e_test.cpp` | M1 through a real `dbus-daemon` |
| `skip_hang.cpp` | `a()` infinite loop in `skip_value` |
| `skip_e2e.cpp` | daemon rejects `a()`, so the loop is unreachable |
| `ancestor.cpp` | M11 on the write path |
| `read_follow.cpp` | M11 on the read path |
| `identity_alias.cpp` | M12 key collision |
