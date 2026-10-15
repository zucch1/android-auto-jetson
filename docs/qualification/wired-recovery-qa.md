# Wired reconnect and recovery live QA (task 31, phase B)

Phase-A acceptance for task 31 is `ctest -R wired_recovery` (the logic-level
recovery matrix over `aa::app::WiredSession`) plus the module verification
recorded in `.omo/evidence/jetson-android-auto-receiver/task-31-wired-recovery.json`.
This document is the **phase-B procedure**: it drives the REAL approved phone and
the on-target wired receiver through the plan's recovery scenarios and records
per-scenario recovery time and retry counts.

> Phase B is owner/orchestrator-controlled and was NOT run during phase A (the
> Jetson display is DP-1 DISCONNECTED, the on-target receiver binary is not yet
> installable, and the task-29 udev policy was rolled BACK after its own QA
> round). No live recovery claim is made until an invocation writes a PASS
> record.

## Scenario matrix (plan §31)

| Scenario | Stimulus | Phase-A logic (ctest `wired_recovery`) | Phase-B harness scenario |
| --- | --- | --- | --- |
| USB unplug/replug | sysfs `unbind`/`bind` on the phone's USB device on the port | `WiredRecoveryUnplugReplug.*` (link loss + fresh transport; 5 s bound + fail-closed) | `usb_unplug_replug` |
| Receiver restart | bounce the receiver process | `WiredRecoveryReceiverRestart.*` (no stale trust/session; re-admit) | `receiver_restart` |
| Dummy consumer restart | bounce the display process | `WiredRecoveryConsumerRestart.*` (degrade/recover; media path resumes) | `consumer_restart` |
| Malformed input | malformed AAP frame | `WiredRecoveryMalformed.*` (`transport_malformed_frame`/`transport_io`, no delivery) | `malformed_input` (N/A: host-covered, no shell injector) |
| Unknown phone | second unapproved device | `WiredRecoveryUnknownPhone.*` (synthetic identity fails closed) | `unknown_phone` (BLOCKED when the S9 Edge is absent) |
| Phone lock/unlock + AA restart | ADB keyevent / `am force-stop` | `WiredRecoveryPhoneLockUnlock.*` (degrade/recover + link restart) | `phone_lock_unlock`, `phone_aa_restart` |

## Prerequisites (once per target)

1. Task-29 udev policy installed: `sudo -n bash tools/usb-policy/install.sh`
   (the policy is NOT installed today - it was rolled back after task-29 QA).
2. Approved phone on the qualified port (default `1-2.1`), phone mode before the
   AOA switch, and the approved-phone trust store loaded (task 27).
3. Display connected (DP-1/HDMI-A-1 `connected`) and a PipeWire output sink.
4. An on-target `aa-wired-receiver` binary (see
   `docs/qualification/wired-live-qa.md`); it is native-only today (todo 41).
5. Passwordless `sudo` for the sysfs `unbind`/`bind` step and an authorized ADB.
6. The phone actively projecting video and playing audio during the run: the
   media PASS gates observe real delivery (painted frames, a successful audio
   pump, a delivered input event) and cannot be satisfied by an idle session.

## One documented command sequence

From the development checkout (bounded ssh, fail-closed):

```bash
tools/wired-qa/run-wired-recovery-qa.sh --readiness-only \
    --out /tmp/task-31-wired-recovery.json

tools/wired-qa/run-wired-recovery-qa.sh \
    --receiver /opt/aa/aa-wired-receiver \
    --display  /opt/aa/aa-dummy-display \
    --store    /home/zucchi/.local/state/android-auto-receiver/approved-phones.json \
    --out      /tmp/task-31-wired-recovery.json
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

- `recovery_ms` per scenario: wall time from the stimulus to projection observed
  again, compared against the wired 5 s reconnect budget (see the named
  limitation on budget honesty below).
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
- **Telemetry evidence coverage**: 6 of the 16 host-side cases assert the
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
  wall-clock `recovery_ms` against `BUDGET_MS` on every PASS (a miss records
  `FAILED` `recovery_exceeded_budget`, never PASS), so a phase-B PASS record
  carries the harness-measured budget evidence; the bounded waits remain the
  measurement authority for the observation windows.

## Budget caveats carried from task 9 (do NOT claim)

Task 9 is reduced-scope (owner-signed): the decode p95 latency budget
(<=33 ms at 1280x720@30) and the software H.264 fallback qualification are
**withdrawn**. Phase B must not claim them. A/V-sync and audio-latency budgets
are measured in task 32.

## Privacy

Device serials/MACs/IPs are never written to committed content or the receipt;
the harness redacts before export. Raw captures stay under `/tmp` on the host.
