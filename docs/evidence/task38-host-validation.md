# Task 38 host validation

The current source verification is for `bf4cb096b0bf5c44a51a4ac953291b60c0f3b3a1`.
The latest fetched main is `4c906e12781cf6a639eb4ebef26f8a9dc18db039` and is an ancestor.
No rebase or force-push was used. Builds stayed under this worktree, with at most
four jobs, one C++ build at a time, using existing staged dependencies.

## Current continuation

- Full host-dev build: passed (`ninja: no work to do`; current binaries already built).
- Nine focused CTest entries: **9 passed, 0 failed**, 155.13 seconds. They cover
  helper unit, bond restart, BlueZ runtime, pending-enrolment retirement, composed
  helper runtime, helper security, wireless composed TLS/admission and receiver smoke.
- ComparisonControl plus AcknowledgementOrder: **30 GoogleTest cases passed**.
- The complete suite was **not rerun at this final source revision**. It currently
  registers 592 entries. Do not interpret historical totals below as a final-head
  complete-suite pass.

The four inherited follow-up commits prevent Just Works/non-comparison acceptance,
consume comparison consent once, preserve failed-removal cleanup over helper restart,
separate bond verification from acknowledgement, reconcile receiver state before
and after acknowledgement, track retirement after a failed acknowledgement/store
write, and disconnect the device before attempting removal.

## Ordering and explicit requirement exception

No approval or durable managed-bond authority is recorded until explicit numeric
comparison acceptance and exact helper bond verification/acknowledgement. Receiver
checks run before acknowledgement and again before the shared approval write.
Timeout does not accept comparison. The seven required negative cases are covered
by ComparisonControl tests and the private-bus retirement fixture.

**Exception to the literal “nothing durable” requirement:** the inherited hardening
persists a cleanup-only `pending-removals` journal before opening admission. It is
not trust, managed-bond authority, or permission to connect. It exists so a helper
restart cannot lose a failed removal. This exception is retained and disclosed for
review; the PR must not claim that absolutely no durable state exists before consent.
The enrolment request itself remains in memory.

| Failure case | Regression |
|---|---|
| Rejection | ComparisonControl.RejectionLeavesNoApprovalOrPending; private-bus reject |
| Cancellation | ComparisonControl.CancellationLeavesNoApprovalOrPending; cancel-retry |
| Timeout | ComparisonControl.TimeoutLeavesNoApprovalOrPending; real 60-second BlueZ expiry |
| Disconnect | ComparisonControl.DisconnectLeavesNoApprovalOrPending; private-bus disconnect |
| Helper failure | ComparisonControl.HelperFailureAfterAcceptanceLeavesNoApprovalOrPending; helper-failure |
| Receiver restart | ComparisonControl.ReceiverRestartLeavesNoApprovalOrPending; abrupt receiver death |
| Store reload | ComparisonControl.StoreReloadInvalidatesPendingEnrollment; private-bus reload |

On the original resolved merge `363ea0d`, the seven negative-case regressions and
corrected pre-acceptance assertion produced **8 failures / 19 comparison tests**.
The current 30 cases additionally cover the acknowledgement ordering and retirement
failure windows. Private-bus tests also cover helper restart, Just Works rejection,
and store-write failure with and without a removal retry.

## Earlier whole-suite and integration evidence

These are exact counts from saved logs, before the latest four follow-up commits:

| Run | Entries | Passed | Failed/timed out |
|---|---:|---:|---:|
| Full sequential suite before subsequent main merges | 551 | 494 | 57 |
| Post-merge affected suites | 124 | 115 | 9 |
| Focused correction rerun | 13 | 8 | 5 |
| Final-main protocol/session/video/receiver checks | 114 | 105 | 9 |
| Regenerated audit tests | 38 | 38 | 0 |

The 600-second IPC benchmark passed in 600.08 seconds. Across these saved runs,
591 distinct test names were exercised; their latest historical results were
569 passing and 22 failed/timed out. The earlier PR body also records an intervening
validation and fresh-main comparison by the previous implementer; those results
were not re-executed in this continuation and are not substituted for the logs here.

## Every observed failure and main comparison

The table includes transient failures later resolved, not just the 22 remaining in
the historical ledger. **Current-main execution comparison was not performed in
this continuation.** The supplied `~/aa-build-main/SOURCE_SHA` is the stale
`dd013ecb0e28fae346e4d665af521d52ef66c756`. Therefore this report does not assert
that a failure also occurs on current main. The previous PR validation reported
matching environmental failures on a fresh main snapshot, but its raw comparison
logs were not available here to independently refresh each row.

| Test | Latest saved historical result | Evidence / reason | Current-main execution comparison |
|---|---|---|---|
| `consumer_decode_build_overlay_tamper` | Failed | Required /usr/aarch64-linux-gnu/include cross headers absent. | Not refreshed / could not compare using the supplied stale baseline |
| `consumer_decode_build_sysroot_tamper` | Failed | Required /usr/aarch64-linux-gnu/include cross headers absent. | Not refreshed / could not compare using the supplied stale baseline |
| `consumer_decode_fixture` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `decode-cross-root-overlay` | Failed | Required /usr/aarch64-linux-gnu/include cross headers absent. | Not refreshed / could not compare using the supplied stale baseline |
| `decode-probe-corrections` | Failed | Worker exited before the expected /proc children file could be read. | Not refreshed / could not compare using the supplied stale baseline |
| `decode-probe-smoke` | Failed | Probe stdout did not decode as the expected JSON. | Not refreshed / could not compare using the supplied stale baseline |
| `input_sensors_memory` | Failed | Valgrind unavailable; mandatory memory qualification sentinel fails. | Not refreshed / could not compare using the supplied stale baseline |
| `media_decode_probe_suite` | Failed | Probe stdout did not decode as the expected JSON. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_audit_baseline` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_audit_genuine_rename_still_matches` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_audit_message_path_shape_boundary` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_audit_renamed_enums_path_only` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_audit_repin_comment` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_audit_sentinel_false_match` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_audit_zero_only_enum_false_match` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `protocol_adapter_protobuf_audit` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_bluez_lifecycle_unregister` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_bluez_runtime` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_lifecycle_CompletedApLoss` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_lifecycle_CompletedLifecycle` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_lifecycle_CompletedLoss` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_lifecycle_FailedReplacement` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_nm_ap_private_bus` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_private_bus` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `system_helper_retirement_budget_mutex` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_acquire_blockers` | Failed | Selector assertion for real dpkg directories and ld comments. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_ceiling_budget` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_ceiling_bundle` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_ceiling_main` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_ceiling_production` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_ceiling_window` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_diag_heartbeat` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_sizing_bundle` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_sizing_collector` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_sizing_failure_paths` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_sizing_resources` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_sizing_rss_lifecycle` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `target_safety_sizing_sqlite_stdin` | Failed | Memory/admission or consequent supervisor assertion; guard retained. See saved log for the exact assertion. | Not refreshed / could not compare using the supplied stale baseline |
| `trust_store_bus` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `trust_store_control_flow` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wired_receiver` | Failed | Latest saved historical run: ActualMainRealGstIndependentCadenceReversibleHoldControls stopped during negotiation with no successful audio. An earlier rerun passed. | Not refreshed / could not compare using the supplied stale baseline |
| `wired_recovery_qa_gates` | Timeout | 300-second timeout, reproduced in the focused rerun. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_composed_admission` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_composed_bootstrap` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_composed_consumer_loss` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_composed_coordinator` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_composed_tls` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_helper_CompletedApLoss` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_helper_CompletedLifecycle` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_helper_CompletedLoss` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_helper_FailedReplacement` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_helper_bluez` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_helper_snapshot` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_e2e_retirement_mutex` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_ApOnly` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R01` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R02` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R03` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R04` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R05` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R06` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R07` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R08` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R09` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R10` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |
| `wireless_recovery_R11` | Passed after correction | Resolved by integration/fixture/audit corrections in the saved runs. | Not refreshed / could not compare using the supplied stale baseline |

## Scope and qualification

Receiver and HFP changes are retained; no feature was removed or disabled. Their
current focused integration checks pass. The historical failures above remain
visible and are not waived by changing assertions, warning policy, or resource guards.

Not verified: a phone, handset comparison UX, installed `/usr/libexec/aa-system-helper`,
real BlueZ/controller key persistence and removal, live HFP interoperability, AP
association or wireless projection. No installation, service restart, sudo, real
Bluetooth mutation, or phone test was performed by this continuation. Issue #72
remains open. The separate #112 store-failure/cancellation follow-up is not merged
into this fetched main; the shared persistence routine remains isolated for it.

Local uncommitted logs are copied under `build/host-dev/task38-validation/`.
