# Task38 wireless reconnect and recovery evidence

Status: **real-device qualification pending**. Historical host evidence remains
host-only, not production wireless admission or full wireless compliance. The
authenticated type5 bond has survived reconnect, but HFP presence still fails;
there is no passing real-phone baseline or full-scope qualification receipt.
The schema support below does not change those outcomes. Parent-supplied actual
target receipts and explicit owner sign-off are still required for closure.

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
python3 -B tests/wireless_recovery/test_full_scope_qa_gates.py \
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

`run` now emits **schema v2**, `qualification_scope: full-task38`, an empty
`device_evidence` object, and `full_scope_signoff: null`. It runs no phone/target
commands and cannot populate device results or infer approval. A successful
`validate` exit means the record is internally valid, **not** that its status is
complete; a valid `failed` or pending record remains failed or pending. Use a
new evidence output path: never overwrite an older qualification bundle.

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

## Historical v1 blocked and withdrawn claims

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

## Versioned full-task38 schema (v2)

Absent `schema_version` means v1; explicit `schema_version: 1` is also accepted.
Its existing statuses, required withdrawals, B01-B03 blocked rules and exact-head
reduced-scope sign-off remain unchanged. Unknown versions are rejected. Existing
artifacts are not migrated, edited or retroactively qualified. To evaluate new
target work, create a separate v2 bundle and preserve the original v1 bundle.
`base_document()` remains a legacy-compatible builder;
`base_document(full_scope=True)` creates a pending v2 envelope.

V2 adds `schema_version: 2`, `qualification_scope: full-task38`,
`device_evidence`, and `full_scope_signoff`. It supports `failed`,
`pending-owner-signoff`, `completed-reduced-scope`, and `completed-full-scope`.
Missing device records are allowed while pending. Present non-pass records use
`status: pending|blocked|fail` and a nonempty `reason`. A failed device record
requires overall `failed`, even if all host assertions pass. A reduced-scope
record must name each missing/non-pass device requirement and the host R07
limitation in `withdrawn_claims`, and obtain the existing exact-head owner
approval covering those withdrawals. Reduced scope is not full-task38 closure.

Full-scope completion requires **all** of:

1. The unchanged host R01-R11 gates, including the intentional blocked R07
   cleanup-negative case and its separate passing protocol observations.
2. Passing on-device records for `baseline`, **R01-R11**, **B01-B03**, and
   `note9_rejection`, with every required artifact present and digest-matched.
3. No withdrawn required claims, `worktree_dirty: false`, and a known evaluated
   `git_commit` matching every device receipt.
4. `full_scope_signoff` containing explicit owner `approver`, `date`, and
   `pr_head` equal to that evaluated commit; `reduced_scope_signoff` stays null.

The stricter target R01-R11 receipts prevent a host-double result being silently
relabelled target qualification. The host R07 negative-control result is **never
promoted**. Its presence keeps the existing host-derived
`no_ap_leak_verified=false` and `hard_kill_cleanup.status=blocked`, even in a
complete synthetic schema fixture. Actual target R07 cleanup is recorded
separately under `device_evidence.R07`; without independently proved AP-off,
nft removal and BlueZ cleanup it is blocked, and full-scope completion fails.
This does not add a crash-durable ledger claim or prove generic AP safety.

### Device receipt format

Every passing record contains:

- `status: pass`, `source: on-device`, evaluated `git_commit`, `timestamp`,
  pseudonymous `target_alias`, `phone_alias`, and `run_id` linking the sanitized
  receipts to the run. Target alias matches baseline; the approved phone alias
  also matches baseline except for rejection cases R10 and `note9_rejection`
  (the latter's alias is `Note9`).
- `observations`: the scalar results below, measured from the same monotonic
  stimulus t0 within each scenario, never from delayed detection or RPC reply.
- `artifacts`: a map from required artifact kind to
  `{"path": "relative/sanitized-receipt.txt", "sha256": "<64 hex characters>"}`.
  Paths resolve relative to the evidence JSON's directory, must stay inside it,
  and must identify nonempty files whose SHA-256 matches. The validator neither
  contacts the target nor generates receipts. Do not include raw identifiers,
  PSKs, credential files, or raw captures; use sanitized exports/digests.

Every pass requires `trust_store_unmodified`, `identity_fence`,
`generation_fence`, `owner_fence`, `source_fence`, and
`final_transmission_fence` equal to boolean true, supported by the receipts.
`freshness_elapsed_ms` is a nonnegative integer **strictly below 950**. No lease,
trust, ownership, generation, or final-send fence is relaxed by this schema.
Admission closure still includes detection and cancellation and is **<=1000 ms**.

| Record | Required observations beyond the common security evidence | Additional artifact kinds |
|---|---|---|
| All records | Common receipt metadata and security/freshness observations | `stimulus`, `services`, `bluez`, `nm`, `nft`, `trust` |
| `baseline` | `paired`, `bonded`, `authenticated_type5`, `hfp_present`, `wpp_completed`, `ap_active`, `tcp_connected`, `aap_authenticated`, `confirmed_active`, `video_delivered`, `audio_delivered`, `input_delivered` all true; integer `rfcomm_channel=8`; `bootstrap_elapsed_ms` and `aap_auth_elapsed_ms` each <=10000 | `hfp`, `rfcomm_wpp`, `projection`, `consumer` |
| R01-R11 | `admission_closed_ms<=1000`, `terminal_recovery_ms<=15000`, terminal `ACTIVE` or `FAIL_CLOSED`; link retirement <=5000 except R08 grace and R10 rejection; independent old-AP inactivity for R03-R09 and teardown start except SIGKILL; old/new activation distinction for activation scenarios | Common artifacts showing the actual scenario stimulus and resulting cleanup |
| R10 rejection | Unknown/unapproved peer, no credential release, session creation or persistence; `FAIL_CLOSED` <=1000, as for Note9 | Common artifacts |
| R07 target cleanup | `independent_ap_inactive`, `owned_nft_removed`, `bluez_cleanup_verified` all true in addition to timing/activation evidence | `independent_ap_off` |
| R08 target grace | Expiry, teardown start and link retirement in `[10000,11000)`; old AP inactive by `min(teardown_started_ms+5000,15000)` | Common NM evidence for the captured old activation, not a deactivation acknowledgement |
| B01 | `stimulus=adb-lock-unlock`, admission <=1000, link retirement <=5000 and terminal <=15000 | `adb`, `projection` |
| B02 | `stimulus=adb-aa-restart`, admission <=1000, link retirement <=5000 and terminal <=15000 | `adb`, `projection` |
| B03 | `stimulus=bluetoothctl-disconnect-block`, `disconnect_exercised=true`, `block_exercised=true`, admission <=1000, link retirement <=5000 and terminal <=15000 | `bluetoothctl`, `projection` |
| `note9_rejection` | `approved_phone=false`, `credentials_released=false`, `session_created=false`, `phone_persisted=false`; terminal `FAIL_CLOSED` and rejection <=1000 | `rejection` |

Every non-baseline terminal `ACTIVE` requires `fresh_bootstrap`,
`confirmed_active`, and video/audio/input delivery true. `FAIL_CLOSED` requires
`admission_closed=true`; it is not projection recovery. R11 specifically must
reach ACTIVE. B01-B03 scenario summaries must match their device records'
status, terminal state and protocol elapsed time; blocked summaries retain a
reason. Ordinary assertion failures are fail, not a hardware-blocked exception.
Activation scenarios retain the captured old activation; a new activation is
required only when claiming ACTIVE recovery, not for verified fail-closed cleanup.

The validator checks completeness, budgets, consistency and file integrity. It
cannot establish that arbitrary file contents describe a real phone or that a
human sign-off is authentic. Owner review of the sanitized source receipts is
therefore required; hashes are not independent hardware attestation. The new
`test_full_scope_qa_gates.py` fixtures are explicitly **SYNTHETIC SCHEMA TEST
ONLY** and temporary. Their accepted full-scope shape is not an actual case
pass, device qualification, or completion claim. No real-phone result has been
created or upgraded by these tests.
