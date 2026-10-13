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
`BUDGET_MS` (default 5000 ms). Exit 0 requires the D-Bus projection contract to be
re-observed after every disturbance. The sysfs `unbind`/`bind` step is bounded and
reversible; the phone returns to normal operation.

## Observables recorded (fed into the task-31 receipt)

- `recovery_ms` per scenario: wall time from the stimulus to projection observed
  again, compared against the wired 5 s reconnect budget.
- `retries`: the highest `"retries":N` reported by the receiver's redacted
  `reconnect` diagnostics after the disturbance.
- Scenario `status`: `PASS` (recovered within budget), `FAILED` (did not recover
  or partially recovered), `BLOCKED` (prerequisite missing), or `N/A`.

## Budget caveats carried from task 9 (do NOT claim)

Task 9 is reduced-scope (owner-signed): the decode p95 latency budget
(<=33 ms at 1280x720@30) and the software H.264 fallback qualification are
**withdrawn**. Phase B must not claim them. A/V-sync and audio-latency budgets
are measured in task 32.

## Privacy

Device serials/MACs/IPs are never written to committed content or the receipt;
the harness redacts before export. Raw captures stay under `/tmp` on the host.
