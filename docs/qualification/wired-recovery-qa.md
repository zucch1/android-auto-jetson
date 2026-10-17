# Wired reconnect and recovery live QA (task 31, phase B)

Phase-A acceptance for task 31 is `ctest -R wired_recovery` (the logic-level
recovery matrix over `aa::app::WiredSession`) plus the module verification
recorded in `.omo/evidence/jetson-android-auto-receiver/task-31-wired-recovery.json`.
This document is the **phase-B procedure**: it drives the REAL approved phone and
the on-target wired receiver through the plan's recovery scenarios and records
per-scenario recovery time and retry counts.

> Phase B is owner/orchestrator-controlled and remains BLOCKED/not performed in
> this local fix round. Historical device-readiness observations are not current
> device observations. No live recovery or soak claim is made by host tests.

## Scenario matrix (plan §31)

| Scenario | Stimulus | Phase-A logic (ctest `wired_recovery`) | Phase-B harness scenario |
| --- | --- | --- | --- |
| USB unplug/replug | sysfs `unbind`/`bind` on the phone's USB device on the port | `WiredRecoveryUnplugReplug.*` (link loss + fresh transport; 5 s bound + fail-closed) | `usb_unplug_replug` |
| Receiver restart | bounce the receiver process | `WiredRecoveryReceiverRestart.*` (no stale trust/session; re-admit) | `receiver_restart` |
| Dummy consumer restart | bounce the display process | `WiredRecoveryConsumerRestart.*` (degrade/recover; media path resumes) | `consumer_restart` |
| Malformed input | malformed AAP frame | `WiredRecoveryMalformed.*` (`transport_malformed_frame`/`transport_io`, no delivery) | `malformed_input` (N/A: host-covered, no shell injector) |
| Unknown phone | second unapproved device | `WiredRecoveryUnknownPhone.*` (synthetic identity fails closed) | `unknown_phone` (BLOCKED when a distinct second device is absent) |
| Phone lock/unlock + AA restart | ADB keyevent / `am force-stop` | `WiredRecoveryPhoneLockUnlock.*` (degrade/recover + link restart) | `phone_lock_unlock`, `phone_aa_restart` |

## Prerequisites (once per target)

1. Task-29 udev policy installed: `sudo -n bash tools/usb-policy/install.sh`
   (verify installation at run time; historical installation state is not proof).
2. Approved phone on the qualified port (default `1-2.1`), phone mode before the
   AOA switch, and the approved-phone trust store loaded (task 27).
3. Display connected (DP-1/HDMI-A-1 `connected`) and a PipeWire output sink.
4. An on-target `aa-wired-receiver` binary (see
   `docs/qualification/wired-live-qa.md`); target packaging is task-41 scope.
5. Passwordless `sudo` for the sysfs `unbind`/`bind` step and an authorized ADB.
6. The phone actively projecting video and playing audio during the run: the
   media PASS gates observe real delivery (painted frames, a successful audio
   pump, a delivered input event) and cannot be satisfied by an idle session.

## One documented command sequence

From the development checkout (bounded ssh, fail-closed), first create a private,
durable evidence directory outside disposable worktrees. Use its absolute path
as `CAPTURE_DIR`; retain sanitized results and command logs there with SHA-256
hashes, not only in temporary storage:

```bash
tools/wired-qa/run-wired-recovery-qa.sh --readiness-only \
    --out "$CAPTURE_DIR/task-31-readiness.json"

tools/wired-qa/run-wired-recovery-qa.sh \
    --receiver /opt/aa/aa-wired-receiver \
    --display  /opt/aa/aa-dummy-display \
    --store    /home/zucchi/.local/state/android-auto-receiver/approved-phones.json \
    --out      "$CAPTURE_DIR/task-31-wired-recovery.json"
```

The harness exits non-zero with a `BLOCKED` or `FAILED` result (`blocked_on`)
whenever a prerequisite is unmet or a scenario does not recover within the wired
`BUDGET_MS` (default 5000 ms). Exit 0 requires the disturbance itself to have
been OBSERVED (a state/flow dip - a stimulus that disturbs nothing records
`BLOCKED`, never `PASS`), the D-Bus projection contract to be re-observed after
it, AND real media delivery evidence after each recovery (see Observables).
The sysfs `unbind`/`bind` step is bounded and reversible; the phone returns to
normal operation.

## Observables recorded (fed into the task-31 receipt)

- `recovery_ms` per scenario: invoking-host monotonic time from dispatch of the
  FIRST disruptive command (or admission attempt) to COMPLETE terminal evidence,
  sampled after the last required probe is validated. Recovery requires
  projection plus every scenario-specific predicate and all three media
  conjuncts below. Rejection requires exit 3 plus retrieved, validated second-
  attempt attribution/port and no `ready`/`active`. Remote timestamps cannot
  backdate observation. All six executable scenarios share one absolute
  deadline: integer nanoseconds must satisfy `0 <= elapsed < budget_ms * 1000000`.
  Exactly 5000 ms FAILS; environment/CLI budgets must be in `1..5000`.
- `elapsed_ms`: elapsed scenario observation time, including partial evidence;
  `recovery_ms` stays null without complete terminal evidence. Complete-but-late
  evidence has numeric `recovery_ms` but is FAILED `recovery_exceeded_budget`.
  Pre-stimulus BLOCKED rows have neither timing claim. Clock errors fail closed.
- `wall_ms`: harness entry through completed cleanup, sampled before JSON
  serialization. Setup, reporting and independent safety-timeout cleanup are
  not acceptance evidence; overall wall time can exceed five seconds without
  invalidating a timely complete observation. The test runner also retains
  subprocess wall time through exit.
- `retries` per scenario: the receiver's reconnect-attempt counter as the
  PER-DISTURBANCE delta (counter after the disturbance minus the counter at
  it), not the run-cumulative maximum.
- `evidence` per scenario: what was actually OBSERVED to earn the status. A
  recovery `PASS` requires the disturbance to have been observed (a state/flow
  dip; unobservable stimuli record `BLOCKED` `dip_unobserved`, never `PASS`),
  session re-activation (the receiver's `active` event), and real media delivery
  after recovery - all three classes:
  - **painted frames**: `frames_painted` growth with `render_ok` from the
    shipped dummy display's test-only status channel (`video_sent` alone only
    proves units crossed the channel, not that anything painted);
    all five media-recovery scenarios require a successfully probed, nonnegative
    integer baseline before disturbance. Missing/empty/malformed status or a
    failed probe records `FAILED` `frames_baseline_unavailable`, with
    `frames_painted_baseline: null` (never a fallback `0`) and no recovery-time
    claim. A parsed `0` is valid. Consumer restart also requires a fresh baseline
    from the new display process. Post-recovery status must come from a successful
    probe and contain an integer counter strictly greater than that baseline,
    greater than zero, with JSON boolean `render_ok: true`; unchanged counters,
    negative values, numeric prefixes, and non-integer values cannot pass;
  - **successful audio pump**: a `media_timing` record - the receiver logs
    `audio_chunks` BEFORE `router_.pump()` runs, so `audio_chunks` presence is
    not proof of pump success;
  - **delivered input**: an `input_forwarded` record with `"ok":"true"` - the
    bare event is not proof (`"ok":"false"` means the send failed).

  `GetState=projecting` with no media is NON-PASS
  (`no_media_evidence_after_recovery`). `unknown_phone` likewise requires an
  OBSERVED admission rejection (transport_unauthorized_peer /
  session_unknown_phone / code 302 / connect failure); a quiet log records
  `BLOCKED` `rejection_unobserved`, not a vacuous fail-closed `PASS`.
- Scenario `status`: `PASS` (recovered within budget with full evidence),
  `FAILED` (did not recover, partially recovered, or media evidence missing),
  `BLOCKED` (prerequisite missing or the claim is unobservable), or `N/A`.
- Subset runs: `--only` writes `"only"` and `"subset": true` into the JSON and
  the verdict text names the subset - a subset `PASS` is never mistakable for a
  full-matrix `PASS`.

## Named limitations

- **No outer receiver recovery loop (task 41)**: `tools/wired-receiver` relinks
  on its session thread but has no supervisor restart loop yet. A full
  `receiver_restart` cycle (kill the process, come back, re-project) may record
  `FAILED` against today's binary - the harness fails closed and never masks it.
- **Single-shot relink** (plan scenarios `usb_unplug_replug`, `phone_aa_restart`
  and `phone_lock_unlock`): each of these scenarios exercises exactly ONE
  relink cycle. The receiver's recovery path (`tools/wired-receiver/main.cpp`,
  the `poll_transport()` error handler) attempts exactly one `open_wired_link`
  relink per link loss and fails closed - `session.fail(transport_io)` and the
  session thread exits - when that single relink or its follow-up handshake
  steps fail. Relink storms, retry/backoff, and multi-cycle recovery are NOT
  covered and are not claimed here.
- **Telemetry evidence coverage**: the current host-side matrix has 20 cases;
  the original six capture-asserting cases include the
  redacted diagnostics capture (the unplug_replug recovery pair
  `FreshTransportRecoversToActiveWithinWiredBudget` and
  `ReconnectFreshCipherRequiresFullHandshake`, `DegradeThenRecover...KeepsRouting`,
  `RecoverAfterRevokeFailsClosedWithTelemetry`,
  `UnknownOnReconnectFailsClosedWithTelemetry`, and
  `LockUnlockAndPhoneSideRestartRecover`); the `receiver_restart`, `malformed`
  and `real_clock` families assert behavior and counters only. No per-case
  telemetry-assertion claim is made for those families.
- **5 s budget honesty**: the budget is asserted EXACTLY only on the
  deterministic fake clock (recover-to-active inside the bound; fail-closed
  typed `timeout` at exactly 5000 ms). The real-clock case
  (`RecoveryCompletesBoundedWorkOnSteadyClock`) is a self-measured wall-clock
  smoke ceiling and does NOT certify the budget. The harness ENFORCES its own
  host-monotonic terminal-observation deadline on every PASS (expiry records
  `FAILED` `recovery_exceeded_budget`, never PASS). Projection alone is partial
  evidence. Probe windows, including `EVIDENCE_WAIT_MS`, can shorten but never
  extend that absolute deadline. This locally tested harness behavior is not
  a claim of live device acceptance.
- **Integrated receiver boundary PENDING**: handwritten hermetic rejection
  records test harness parsing, not production receiver serialization. The
  actual rejected-attempt and pre-admission-failure child-process tests belong
  on the integrated task-30/task-31 receiver seam, not this standalone lane.

## Budget caveats carried from task 9 (do NOT claim)

Task 9 is reduced-scope (owner-signed): the decode p95 latency budget
(<=33 ms at 1280x720@30) and the software H.264 fallback qualification are
**withdrawn**. Phase B must not claim them. A/V-sync and audio-latency budgets
are measured in task 32.

## Privacy

Device serials/MACs/IPs must not enter committed content or the receipt. The
harness exports a host hash and pseudonymous identity/port evidence. Keep raw
captures private, sanitize exported logs, and retain the sanitized evidence in a
mode-0700 durable directory outside disposable worktrees with a SHA-256 manifest.
Do not bind receipts solely to `/tmp`: missing historical bytes must be disclosed
as unavailable, not replaced by their recorded hashes. Local harness coverage,
integrated receiver-boundary coverage, and live/soak acceptance are separate.
