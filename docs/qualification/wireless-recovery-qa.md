# Task38 wireless reconnect and recovery QA (host)

Status: **host matrix green, `pending-owner-signoff`**. This is not production
wireless admission, real-phone qualification, or full wireless compliance.
Overall closure can only become `completed-reduced-scope` after explicit owner
sign-off naming the exact withdrawn claims and final PR/head.

## Run

```sh
cmake --build build/host-dev -j2
ctest --test-dir build/host-dev -R wireless_recovery --output-on-failure
python3 -B tools/wireless-qa/wireless_recovery_qa.py run \
  --build build/host-dev \
  --out .omo/evidence/jetson-android-auto-receiver/task-38-wireless-recovery.json
python3 -B tools/wireless-qa/wireless_recovery_qa.py validate \
  .omo/evidence/jetson-android-auto-receiver/task-38-wireless-recovery.json
python3 -B tests/wireless_recovery/test_recovery_qa_gates.py \
  tools/wireless-qa/wireless_recovery_qa.py
```

The runner executes each registered scenario with `GTEST_OUTPUT`, converts the
recorded monotonic observations to JSON, and validates that JSON before
returning success. Evidence and scenario logs stay uncommitted. The validator
has mutation controls for missing scenarios, missing teardown-start evidence,
late AP-off, missing old/new activation distinction, unsupported AP-leak
claims, unsigned reduced-scope closure, promoted blocked scenarios, and missing
R11 media evidence. Per-case status, nonnegative timings, aggregate/case
consistency, blocked-cleanup case status, and required cleanup observations are
also validated.

## Matrix and terminal claims

All timings are measured from one monotonic fault-injection `t0`; current
measurements are in the JSON, not hard-coded here. Admission closure is at most
1 s, surviving link teardown at most 5 s, and terminal recovery/fail-closed
state at most 15 s unless the confirmed-consumer grace applies.

| Scenario | Host result and terminal claim |
|---|---|
| R01 `Connected=false` | Old handle stays retired after `Connected=true`; explicit fresh reconnect admits. |
| R02 BlueZ owner/name loss | Authority fails closed; restoring the name alone does not revive it. |
| R03 NM AP teardown | Admission closes; acknowledged State3 deactivation with the interface still attached is not AP-off; later State4 plus `Device.ActiveConnection=/` and owned nft removal are independently observed. |
| R04 AP generation/endpoint restart | Fresh AP-only grant and fresh bootstrap use a new activation and pinned snapshot on port 5277. |
| R05 fresh receiver objects | Receiver-loss cleanup is observed; the same persistent approval reloads without `AddPhone`. |
| R06 graceful helper SIGTERM/respawn | Graceful NM/nft/BlueZ cleanup is recorded; respawn reacquires AP and admits without pairing. |
| R07 helper SIGKILL/respawn | Protocol recovery/fail-closed is separate from cleanup. Delayed client-bound shutdown exposes State3 before independent State4 plus `Device.ActiveConnection=/` proof. When NM cannot complete shutdown, cleanup is **BLOCKED** as `cleanup-unverified / operator-supervised recovery required`, never passed. |
| R08 confirmed ACTIVE consumer owner loss | Admission closes immediately; grace expiry and actual teardown start are in `[10000,11000)` ms; old AP is independently inactive by `min(teardown_start+5000,15000)` ms. |
| R09 consumer generation replacement | Old admission/epoch retires; stale work cannot reopen it; only the new generation acquires fresh AP and handoff authority. |
| R10 synthetic unapproved/unbonded peer | Rejected before credential release or session creation; this is not production TCP-to-Bluetooth identity proof. |
| R11 full reconnect through projection | Fresh RFCOMM bootstrap, TCP and encapsulated AAP TLS reach confirmed ACTIVE, with video, audio and input evidence, admitted by the production activation-bound predicate. |

R01-R10 except R11 are admission-level recovery/fail-closed claims; only R11
claims full projection recovery. Every recovery uses an explicit fresh
`reconnect`/`begin`; a retired handle never revives. The receiver owns and
reloads `ApprovedPhoneStore`, and approved recovery does not open pairing.

## Blocked and withdrawn claims

- **B01:** real phone lock/unlock through ADB is blocked; no host surrogate is
  claimed.
- **B02:** real Android Auto restart through ADB is blocked.
- **B03:** real `bluetoothctl disconnect`/`block` and Jetson controller/RF and
  BlueZ signal-completeness behavior are blocked.
- Host NM/BlueZ doubles do not qualify real RF, driver channel policy,
  installed-service behavior, or crash-durable managed-peer/revocation state.

The evidence records `no_ap_leak_verified=false` whenever any required cleanup
is unverified, including the intentional R07 unverified-shutdown case. It also
records trust-store preservation, per-case cleanup observations, old/new
activation identifiers, null-field applicability reasons, the bounded-cleanup
owner decision (2026-10-24), and `reduced_scope_signoff: null` until separately
approved. The validator accepts `completed-reduced-scope` only when the
sign-off `pr_head` equals the evidence `git_commit` and its `claims` list
covers every withdrawn claim, and the evidence records `worktree_dirty=false`;
required withdrawn claims cannot be dropped. Only R07 may be `blocked` among
host scenarios.
