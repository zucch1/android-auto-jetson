# Task 5 observability / deadline hardening - implementation report

Scope: local work only in `.omo/worktrees/foundation`. No target contact, no
network, no SSH, no git operations, no writes to `.omo/evidence`. Implements
the binding Oracle spec `task-5-hardening-design-review.json` (nine corrections)
in response to `task-5-launcher-fault-injection-outcome.json` (three stall
arrangements reproduce the historical 0-byte transcript at GNU 960.02 s).
Effect: every future remote stall that receives at least one record lands
between two received records, converting a silent run into a localized
interval. A 0-byte transcript proves neither execution absence nor a channel
dead from entry: it remains "no entry record received", and an interval can
only be localized when records were received (Oracle correction 6).

## Deadline envelope (authoritative allocation)

Single source: `tools/target_safety/deadline.py` (docstring carries this
justification). `stage1.EFFECTIVE_CAPS` is a literal mirror; the envelope tests
(`tests/target_safety/envelope.py: deadline_envelope_invariant`) cross-check
both sides. GNU timeout is unchanged.

| term | seconds | meaning / justification |
|---|---|---|
| startup_bound | 20.0 | local GNU start -> remote supervisor deadline armed. GNU `timeout` starts **locally** (clock-origin problem): local spawn + SSH connect (`ConnectTimeout=10`, one attempt) + remote shell + `python3 -B -` + 193885-byte stdin ingestion + in-memory module load. Healthy-path receipts show ~0.83 s. |
| collection_bound | 900.0 | `kernel.WINDOW_SECONDS` - the guard Watch window, unchanged. Supervisor child budget equals it minus pre-collection setup time already spent (unchanged formula). |
| termination_reap_bound | 30.0 | post-collection cleanup: guard TERM/KILL/reap phase 15 s (internal cap 10 s = `GUARD_TERM_GRACE` 5 + `GUARD_KILL_GRACE` 5) + restoration 15 s (three setting ops x <=5 s). |
| egress_bound | 5.0 | terminal result delivery window `[budget-10, budget-5]`; failure fails closed as `egress_failed_or_budget_exhausted`. |
| scheduling_slack | 5.0 | `[budget-5, budget]`: kernel/SSH jitter margin before the worst-case GNU kill. |
| **total** | **960.0** | == `stage1.SSH_TIMEOUT_SECONDS` (GNU timeout). Equality holds; the slack term is the margin. Do not extend GNU. |

**940-supervisor vs 900-guard, resolved explicitly:**
`SUPERVISOR_BUDGET_SECONDS = collection 900 + termination_reap 30 + egress 5 +
scheduling_slack 5 = 940` and `CLEANUP_HEADROOM_SECONDS = 30 + 5 + 5 = 40`
(both values unchanged from before; the decomposition is now named and
tested). `startup 20 + budget 940 = 960`. The existing phase formulas already
place the phases: guard cleanup deadline `budget-25` (terminate end 915 =
collection end + 15), restore deadline `budget-10` (930), egress deadline
`budget-5` (935, **new** - previously deliver() ran to 940 and raced the
worst-case kill), slack 935..940.

## Mechanisms (Oracle corrections 1-9)

1. **One bounded diagnostic-write discipline** - `egress.py` keeps `deliver()`
   (proven nonblocking fd writes, absolute deadline, partial-write resume) and
   adds `Writer`: one serialized emitter (lock + process-single), small
   independently framed JSONL records, monotonic contiguous sequence numbers,
   explicit failure policy - any failed write (EPIPE, backpressure timeout,
   record left partially written) latches the channel and stops all further
   diagnostic attempts. Sizing never aborts on a broken channel and no
   `BrokenPipeError` is ever uncaught (`ceiling_window.broken_stdout` and
   `ceiling_defects.full_stdout_pipe` contracts kept).
2. **Entry record** - `payload.py:bundle()` hoists `egress` first and emits the
   minimal `{"event": "remote_entry", "pid", "monotonic_ns"}` as the earliest
   executable bounded write (5 s deadline), before any other module source is
   executed. `remote_entry_meta` (boot_id / python_version / executable) is the
   second record. CPython compiles the whole stdin before first execution, so
   the entry record also marks interpreter/source ingestion done.
3. **Stage brackets** - `stage_started`/`stage_completed` pairs with phase ids
   and sequence numbers around: module_load (preamble), setting_read,
   memory_read, admission (via `bootstrap.hold_before_raise` stage sink),
   guard_init (supervisor spawn + guard child), watch_session (guard `Watch.session`),
   collection (supervisor waits), plus explicit `cleanup_started` /
   `cleanup_completed` / `egress_started` transitions and the terminal result
   record. A stream that stops mid-phase leaves the interval open: the last
   received record identifies the interval (tested).
4. **Heartbeat** - integrated into the collector's bounded waits: the selector
   wakes at the earliest of readiness, next heartbeat (30 s, `HEARTBEAT_INTERVAL_SECONDS`)
   or the collection deadline. Records carry worker progress counters
   (records/combined_bytes/stdin_sent) and `last_progress_age_seconds`;
   unchanged values expose worker inactivity. A heartbeat never extends a
   deadline and never counts as progress (tested).
5. **Deadline envelope** - table above; `deadline.py` single-source,
   `stage1.EFFECTIVE_CAPS` mirror, envelope tests cross-check both.
6. **-u not adopted** - the bounded writer is the mechanism; no
   `python -u` change and therefore no regression test claiming its necessity.
7. **Cleanup/egress transitions + receiver validation** - transitions emitted
   explicitly; `report._terminal_complete` validates terminal-record
   completeness on hardened streams (known kinds only, stream discipline,
   cleanup_started -> cleanup_completed -> egress_started exactly once in
   order, single outcome record last, no malformed/truncated line). Receiver
   preserves truncated evidence (raw capture untouched) and marks the stream
   incomplete. Acceptance was **extended only** - legacy outcome-only streams
   keep their exact historical semantics (never weakened).
8. **Diagnostic write failure policy** - explicit (egress.py docstring):
   stop after broken channel; sizing continues its window and cleanup exactly
   as before; an undeliverable terminal record fails closed as rejected
   evidence.

Real hardened supervisor transcript (fixture run, records abbreviated):

```
remote_entry | remote_entry_meta(seq 1) | module_load 2..3 | setting_read 4..5
memory_read 6..7 | admission 8..9 | limit_readiness 10 | ceiling_raise 11
ceiling_raised 12 | guard_init 13..14 | collection 15..16 | guard_result 17
cleanup_started 18 | ceiling_cleanup 19 | cleanup_completed 20
guard_event 21..30 (forwarded child transcript) | egress_started 31
sizing_outcome 32 (terminal, last)
```

## Changed files (before sha256 -> after sha256)

Before hashes are the binding values (worktree matched the binding exactly
before this work). Pure LOC per `qualification.source_checks` (<250 gate).

| file | before sha256 | after sha256 | pure LOC |
|---|---|---|---|
| tools/target_safety/bootstrap.py | `26f3c45bdc7f9a57f3d248f020f6295f8b352de1b16ae10d3ad6db8014f0b475` | `1a7fdaeb9874d3b27a94a2ba789b7e9e8bacb37ab8f2961c20ca07150d59f0c3` | 72 -> 87 |
| tools/target_safety/collector.py | `02898a0de0321f6a2f609c5c084dab64ede24508e262d83e7f7020692dfb5fc8` | `43cb6099d67b4e528f0d8efcfdbf3902a8a93c60ebcc02c936fe85d630d0fc60` | 210 -> 237 |
| tools/target_safety/egress.py | `b03c1e6e203f5bc15dd096a63279040aa3de46c31b115bb116c04633e85a4422` | `a18a6768b1a7785e74b6c6b1dad1c09e61c59ace22ca5cd134628b41fdc06b20` | 38 -> 187 |
| tools/target_safety/guard.py | `1eb37675ea09251944dc10314dc711fff7efcf943fd50f347a2a2be476789549` | `77553bf80adf6e36ede498524415d3d4212cfdd96b45c9a6d78a28e12c31afaf` | 137 -> 139 |
| tools/target_safety/payload.py | `2bf93ff7fb18acd8abf332eb34c81808c5c71571f58ec6f2d108db2a6c692e11` | `b85e6872d51e14eaa6770100d4832104fa26542ba1b67f4358865f194c3f1d83` | 49 -> 61 |
| tools/target_safety/report.py | `779ab5255321ba87a824cefca89dc50aba9cca03e1f18d6ee39e74b28952566a` | `27dcb22e17f1f12b893077b3ceb966e0a9ea9a4310d28a198e056b7483856b91` | 133 -> 235 |
| tools/target_safety/sizing.py | `86f967e982c02b894cf642d9a6524c2537d4868e560e9092cd1e1abef6734e9d` | `6ca803cbe903325e2cdca713340a387eabf3dedbbbcaaf27401ceafd98b08bfc` | 62 -> 71 |
| tools/target_safety/sizing_validation.py | `9927cdbeae4ced666517ec7300565ce167acd7e1a16aa640291fb4bb3f5e893b` | `b20f001c30339d7bd4203295cbc1ced346e4039704f730673ebcf3540278074a` | 223 -> 248 |
| tools/target_safety/stage1.py | `534339184ff7d23b2af029ab93f6df7119c5aa1e5494e58deaf12b8c98a0f656` | `2914e391ff2ac6e25162df4749d249b9d71effaaca76263f3ae60c740f434b05` | 237 -> 248 |
| tools/target_safety/supervisor.py | `dbe98c63e35793ee37bd8cf12e4c2062c7bd71c23a37ac5a0c25f632e1ae3d76` | `83b175e98d16be8a3421e3c413242d198f4a732d656232ba6068795060f71aaa` | 249 -> 249 |
| tests/target_safety/ceiling_qa.py | `7d40d70ee5cba645d3dd1e6df50d0f6e943ac95ffce19016687994de4b5a51d3` | `f7836477514fbc2791e17cb20ff6081acbf81a4ca41e261065ae5d8bd6e14351` | +1 line (suite registration) |
| tests/target_safety/envelope.py | `3352e39a174c8e4107445e00eb0deb62d93d508969ab6b9acde483ba80705a77` | `de30f9a6a7f4d71e939bceb96133b7f8e3af878b416178f49d790bf3cf388fc2` | 148 -> 183 lines (new envelope mirror keys + invariant test) |

Structural note: supervisor.py was at the 249-LOC gate, so the guard-evidence
parsing and collection/resource wire builders moved into `report.py` (their
natural home) and the envelope allocation into `deadline.py`. No behavior
change beyond the hardening itself.

## New files

| file | sha256 | pure LOC | purpose |
|---|---|---|---|
| tools/target_safety/deadline.py | `dcf8c2d995732b07e4e86c3062f9907f3f5bd64314f566c0397c25651410994c` | 40 | authoritative deadline envelope allocation |
| tests/target_safety/diag_writer.py | `f6e97abca16920b3f3bfd47e8b3956854b2cc36a090be4e2ab09f290eb0b2fe2` | 133 | writer: partial writes, EPIPE policy, backpressure deadline, serialization, truncation marking |
| tests/target_safety/diag_heartbeat.py | `7b444fdd1d5ad1c55a8e9391763befa593d51eb195324ac14e9aeafd8fcf4d97` | 84 | heartbeat-in-wait: wake bounds, progress counters, deadline neutrality |
| tests/target_safety/diag_stream.py | `1571b8f691cd911f5fec751097027fb355796ca3deec730e814991eff1c15a7f` | 133 | bundle entry ordering + terminal-record completeness |

## Source vectors (binding method: sha256 of "\n".join("path:sha256" lines),
tests/target_safety sorted then tools/target_safety sorted)

- **69-source set (binding file list, recomputed): `8bbecbde6e25208312fcfc375115125376d5bc44bf468649c5f90cf0cb615782`**
  (was `31e658c1cf4ddc1c4cacd886f2385aba1fa909397d38d123d470a0df5a80f933`)
- Full current set incl. the 4 new files (73 sources):
  `8a45f11f221c6385d0fad886c5e56494f4f0fe09325365f1bd0bc090d7cd9779`

Production bundles (bytes / sha256):

- default guard `bundle()`: 84554 / `c4beec2a7499f34b28ced12647469705910683737364d9200e4901c2df289a86`
- sizing guard: 73440 / `f2fa4b57d7b938ac44bb7919a5c85d4f5658c2ac2e68a0fa224b951a7e40dcbc`
- supervisor: 240977 / `fbe66bb1a49cc3f6fcbd19b7a638237c61fc0cec1809e6c4f0bd72eb800b7219`
- sizing (topology): 229627 / `f59bf7a9f70cd29effc959f431f3303c386a8330f4806676f4af2c9cbc9c3630`

## Test results

Every suite run as `timeout --signal=TERM --kill-after=5s 120s python3 -B
tests/target_safety/<suite>.py`; PASS lines counted from output. **All exit 0.**

| suite | exit | PASS lines | suite | exit | PASS lines |
|---|---|---|---|---|---|
| capacity | 0 | 10 | sizing_admission | 0 | 10 |
| ceiling | 0 | 7 | sizing_bundle | 0 | 3 |
| ceiling_argv | 0 | 9 | sizing_callback_timing | 0 | 4 |
| ceiling_attempt | 0 | 5 | sizing_collector | 0 | 7 |
| ceiling_budget | 0 | 6 | sizing_completion_integrity | 0 | 8 |
| ceiling_bundle | 0 | 3 | sizing_failure_paths | 0 | 10 |
| ceiling_capacity_route | 0 | 5 | sizing_outcomes | 0 | 7 |
| ceiling_capture_paths | 0 | 4 | sizing_reading_validation | 0 | 4 |
| ceiling_defects | 0 | 6 | sizing_resources | 0 | 5 |
| ceiling_durable | 0 | 7 | sizing_route | 0 | 12 |
| ceiling_guard | 0 | 5 | sizing_rss_lifecycle | 0 | 10 |
| ceiling_main | 0 | 6 | sizing_selector_failures | 0 | 10 |
| ceiling_policy | 0 | 11 | sizing_sqlite_absence | 0 | 12 |
| ceiling_production | 0 | 1 | sizing_sqlite_inotify | 0 | 4 |
| ceiling_stdin | 0 | 5 | sizing_sqlite_route | 0 | 5 |
| ceiling_two_links_route | 0 | 5 | sizing_sqlite_stdin | 0 | 5 |
| ceiling_window | 0 | 8 | sizing_sqlite_wire | 0 | 5 |
| diag_heartbeat (new) | 0 | 3 | window | 0 | 4 |
| diag_stream (new) | 0 | 5 | envelope | 0 | 5 |
| diag_writer (new) | 0 | 6 | namespace | 0 | 25 |
| scenarios | 0 | 67 | receipt | 0 | 4 |
| ceiling_qa (aggregate) | 0 | 296 | | | |

`ceiling_capture.py` (QA capture tool, not a standalone suite - takes an
existing QA dir) run as an extra gate: exit 0, 296 pass lines, `source_checks`
clean (AST + <250 pure-LOC gate over all sources), historical evidence byte
hashes unchanged during QA. `tests/target_safety/ceiling_capture.py` is the only
file under tests/ without a standalone main-mode suite (it requires
`sys.argv[1]`, per its own header).

## Limitations (explicit)

1. **No type-check pass is claimed.** basedpyright is unavailable (previously
   declined) and was not installed (`lsp_status`: no Python LSP installed).
   Verification is AST parsing (`qualification.source_checks`), the <250
   pure-LOC gate and the local suites above. No type-error suppression
   (`as`/ignore) was introduced anywhere.
2. **The entry marker cannot prove execution absence on 0 bytes.** A dead or
   blocked channel swallows even the bounded entry write; 0 bytes remains
   "no entry record received", never "execution absent" (Oracle correction 2).
3. **Startup bound is documented, not remotely enforced.** The remote cannot
   observe the local GNU clock; `STARTUP_BOUND_SECONDS=20` is an envelope
   assumption (healthy path ~0.83 s). A startup overrun now fails visibly
   (incomplete stream, no terminal result) instead of silently racing the kill.
4. **The ceiling/prerequisite guard's own evidence records (`probes.py`
   `emit`) still use `print(flush=True)`** - probes.py was outside the declared
   edit scope. Its payload preamble entry/meta/module_load records do use the
   bounded discipline; the strict terminal-completeness validation applies to
   the supervisor stream in both modes.
5. **Egress window is 5 s.** A terminal record that cannot be delivered in
   `[budget-10, budget-5]` (ceiling wire embeds raw guard stdout, bounded by
   the 16 MiB collection cap) fails closed as
   `egress_failed_or_budget_exhausted`: safe, but an oversized evidence payload
   becomes a rejected run rather than a slow one.
6. **Single emitter per process.** The writer is process-single (the bundled
   preamble shares it with the run) and latches broken for the process
   lifetime. In-process multi-run test processes share that latch; no suite
   asserts captured records after a deliberately broken-stdout run (verified).
7. **Heartbeats prove emitter responsiveness, never "working"** (Oracle
   correction 6). Unchanged progress counters localize inactivity to an
   interval but cannot attribute it (worker vs transport) alone.
8. **Live localization is supervisor-level; child localization is retrospective**
   (Oracle correction 4 / D4). Child stage records are forwarded into the
   supervisor stream after collection and cleanup, not live. Live stream records
   localize supervisor stages; child execution progress is reconstructed from the
   collected child transcript.
9. **Cleanup bracket covers supervisor cleanup; window-end reap runs in wait()**
   (Requalification observation 2). The guard child TERM/KILL/reap runs inside
   `guard.wait()` before `stage_completed('collection')`, because output collection
   is incomplete until the child process exits and is reaped. The `cleanup_*`
   transitions bracket supervisor cleanup (idempotent termination escalation,
   setting restoration, reconciliation).
10. **Legacy acceptance semantics preserved by design.** Terminal-record
    completeness is required once diagnostic records appear; a synthetic stream
    carrying only an outcome record keeps its historical meaning. The transport
    and its exclusive local capture remain the trust boundary, unchanged.
11. **No sizing semantics changed**: counters, coverage/protect behavior,
    SQLite absence contract, namespace rules and the report gate semantics are
    untouched; acceptance was extended for terminal-record completeness only and
    never weakened. Watch/session cleanup guarantees preserved (broken-stdout,
    signal and full-pipe regression suites green).
12. **No fresh sizing attempt is authorized by this work.** Per the Oracle
    `fresh_attempt_gate`: implementation review + local re-qualification (the
    three stall arrangements, mid-payload delivery stall, saturated-output /
    partial-write cases, blocked worker with responsive supervisor,
    worst-case cleanup timing under the hardened payload) + separate
    operational authorization remain required. This worktree change is
    local-only; no target contact occurred and no evidence artifact was
    modified.

## Correction round 2 (Oracle review D1-D6 & Requalification observation 2)

Implements the targeted corrections mandated by the Oracle implementation review
`task-5-hardening-implementation-review.json` and accounts for
`task-5-hardened-requal-outcome.json` observation 2.

### Summary of corrections

- **D1 (terminal delivery cap)**: `Writer.terminal()` capped at
  `min(now + 5.0, stream_deadline)` using `EGRESS_BOUND_SECONDS = 5.0`
  (was `bound=None` which gave it the whole remaining window). Injected-write
  check in `diag_writer.py` verifies a stream with 100 s remaining delivers
  terminal capped at 5.0 s.
- **D2 (phase-aware diagnostic allowances)**: Single-sourced
  `DIAGNOSTIC_ALLOWANCE_SECONDS = 1.0` in `deadline.py`, mirrored in
  `stage1.EFFECTIVE_CAPS`. Synchronous diagnostic writes
  (`stage_completed('collection')`, `guard_result`, `cleanup_started`) are
  bounded by the enclosing phase deadline (`_operation_deadline(restoring=True)`).
  Heartbeat writes in the collector loop use `DIAGNOSTIC_ALLOWANCE_SECONDS` as
  their cap, ensuring the 0.1 s resource-sampling cadence is delayed by at most
  the explicit allowance.
- **D3 (receiver hardened-stream validation)**: Strengthened
  `_terminal_complete` (report.py) and `parse_sizing_child`
  (sizing_validation.py) for hardened streams:
  1. Rejects missing final LF (preserves framing).
  2. Requires `remote_entry` first and `remote_entry_meta` second.
  3. Requires the expected stage set for accepted runs (`module_load`,
     `setting_read`, `memory_read`, `admission`, `guard_init`, `collection`).
  4. Preserves legacy outcome-only acceptance semantics exactly.
  5. Regression tests in `diag_stream.py` verify all Oracle counterexamples
     (missing final LF, no stage brackets, metadata fourth -> rejected;
     legacy outcome -> accepted).
- **D4 (claim-limitation option)**: Explicitly limited the claim: live
  localization is supervisor-level; child-stage localization is retrospective
  via the collected stream. Documented in `report.py`, `guard.py`, and this
  report.
- **D5 (local capture frame-receipt journal)**: Added `FrameJournal` and
  `pump_stdout` in `local_capture.py` / `runner.py`. The transport pump tees
  stdout to the file-backed capture while journaling locally timestamped
  receipts (local monotonic timestamp, byte offset, frame length) for complete
  LF-terminated frames into a `.frames.jsonl` sidecar. Trailing partial data is
  explicitly marked with a `partial_tail` receipt. Journal is capped at 4096
  receipts (`frames_capped`). Preserves file-backed capture, kill-group
  semantics, and owned-timeout behavior. Regression suite in `diag_frames.py`.
- **D6 (report opening overclaim)**: Corrected opening claim: 0 bytes never
  proves dead channel or execution absence; an interval is localized only
  when records were received.
- **Requalification observation 2 (cleanup bracket reap phase)**:
  Documented in `guard.py` and this report: the window-end TERM/KILL/reap runs
  inside `guard.wait()` before `stage_completed('collection')` because output
  collection is not complete until the child process is reaped. The caller's
  `cleanup_*` transitions therefore bracket the supervisor-side cleanup
  (idempotent terminate escalation + restoration + reconcile).

### File checksums and LOC (Round 1 -> Round 2)

| file | Round 1 sha256 | Round 2 sha256 | pure LOC |
|---|---|---|---|
| tests/target_safety/ceiling_qa.py | `7d40d70ee5cba645d3dd1e6df50d0f6e943ac95ffce19016687994de4b5a51d3` | `4caa9339fdc5516c26ddc5e2f60649e35140a2aee36b52b26531df2baaf6f714` | 30 |
| tests/target_safety/diag_frames.py (new in R2) | - | `7ef9f61bff47d59bb2ea28a09b83ef3ffd02b47da125facd9abc378897e67716` | 83 |
| tests/target_safety/diag_heartbeat.py | `7b444fdd1d5ad1c55a8e9391763befa593d51eb195324ac14e9aeafd8fcf4d97` | `64e0a03c595da9a0450e96bbc20dd301618f65f66eaee74fde06f24bb9471dfe` | 117 |
| tests/target_safety/diag_stream.py | `1571b8f691cd911f5fec751097027fb355796ca3deec730e814991eff1c15a7f` | `62c952d686a68d38dac6e837bb1ade6b9c6f7f826cffe58fc6ebd06fa90e9f06` | 169 |
| tests/target_safety/diag_writer.py | `f6e97abca16920b3f3bfd47e8b3956854b2cc36a090be4e2ab09f290eb0b2fe2` | `795d7213bf23677132ce67375d6d12758e21e4b3383724f6d9f68f07aab87375` | 182 |
| tests/target_safety/envelope.py | `3352e39a174c8e4107445e00eb0deb62d93d508969ab6b9acde483ba80705a77` | `59f49bacd608d74cc6249c1a9dda2dc36318f1fea3bcf78ab7cbf92f03ae0672` | 162 |
| tools/target_safety/bootstrap.py | `26f3c45bdc7f9a57f3d248f020f6295f8b352de1b16ae10d3ad6db8014f0b475` | `1a7fdaeb9874d3b27a94a2ba789b7e9e8bacb37ab8f2961c20ca07150d59f0c3` | 87 |
| tools/target_safety/collector.py | `02898a0de0321f6a2f609c5c084dab64ede24508e262d83e7f7020692dfb5fc8` | `6c2c0d0aee83809b3de50d1e11f3fb9cec187e19aec8ca049ade881a17c50eb8` | 237 |
| tools/target_safety/deadline.py | `dcf8c2d995732b07e4e86c3062f9907f3f5bd64314f566c0397c25651410994c` | `8b3fe26f523369f7191b69fc23e3ff9f5b4572fa1a31417890fa028d8073bdf3` | 42 |
| tools/target_safety/egress.py | `b03c1e6e203f5bc15dd096a63279040aa3de46c31b115bb116c04633e85a4422` | `d2762069ea65fc3c1f2f41a108319c85809220f1587e55507b47a00ea47ff805` | 223 |
| tools/target_safety/guard.py | `1eb37675ea09251944dc10314dc711fff7efcf943fd50f347a2a2be476789549` | `acfdee5c23b0fc2d9a6fbfa127bb7fc2b7e5bef561eccf67af342b4a3e06e905` | 145 |
| tools/target_safety/local_capture.py | `17735569d3a99433b254750550e5398783f7e5d08033d0d31f4a0fdd9e0befca` | `c1271c477f971a2cf8d5116457a089f805ee2ef50c388ef51747a346e98ff5c5` | 83 |
| tools/target_safety/payload.py | `2bf93ff7fb18acd8abf332eb34c81808c5c71571f58ec6f2d108db2a6c692e11` | `b85e6872d51e14eaa6770100d4832104fa26542ba1b67f4358865f194c3f1d83` | 61 |
| tools/target_safety/report.py | `779ab5255321ba87a824cefca89dc50aba9cca03e1f18d6ee39e74b28952566a` | `c68f4bb9e4c1b0fc974b6b98e10ff12d2dd803a64ba63c1d80e8086a79379d56` | 239 |
| tools/target_safety/restoration.py | `41506601852ca3ee3ce6627266e62dc10ba7138f94818079c37b56ec8593fa1e` | `411b855fd2c006d00b14ab8de8e981912d2948a736c2fdc51a3bb776b9a2642f` | 90 |
| tools/target_safety/runner.py | `191e9395808139a6950f1bade77373a475dc88638a17b85573dd61b52a555628` | `df76905ca0ca223284ebd074c6d8a3a3c190f4d05f40dc946d055118665ef560` | 154 |
| tools/target_safety/sizing.py | `86f967e982c02b894cf642d9a6524c2537d4868e560e9092cd1e1abef6734e9d` | `6ca803cbe903325e2cdca713340a387eabf3dedbbbcaaf27401ceafd98b08bfc` | 71 |
| tools/target_safety/sizing_validation.py | `9927cdbeae4ced666517ec7300565ce167acd7e1a16aa640291fb4bb3f5e893b` | `1c67cfc12f0e6b9fda7e36c98f39a2cfe94617c04b7d5e5304c0e93855c344a8` | 248 |
| tools/target_safety/stage1.py | `534339184ff7d23b2af029ab93f6df7119c5aa1e5494e58deaf12b8c98a0f656` | `7fc013b62645d5252b2593fb422b61efcf97301d5cf91fdb1287711711536743` | 249 |
| tools/target_safety/supervisor.py | `dbe98c63e35793ee37bd8cf12e4c2062c7bd71c23a37ac5a0c25f632e1ae3d76` | `38631c52f94a8dd3cb1bd11cd32bab360a73a93a5c566c9eeba154c84c805ae6` | 249 |

### Recomputed source vectors

Binding method: `sha256("\n".join(f"{path}:{sha256}" lines))` with `tests/`
sorted then `tools/` sorted (no trailing newline).

- **69-source set (binding file list, recomputed)**:
  `ee4686b294146a3bdbcd6f3af147b4226e2d329f0af6aa01c964e35009fc759e`
  (Round 1: `8bbecbde6e25208312fcfc375115125376d5bc44bf468649c5f90cf0cb615782`,
   original: `31e658c1cf4ddc1c4cacd886f2385aba1fa909397d38d123d470a0df5a80f933`)
- **Full current set incl. 5 new test/deadline files (74 sources)**:
  `7d51af1c60c4de776a7094112f558e573f8d8872b465af886f86dcac7ff1598d`
  (Round 1: `8a45f11f221c6385d0fad886c5e56494f4f0fe09325365f1bd0bc090d7cd9779`)

Production bundles (bytes / sha256):

- default guard `bundle()`: 87745 / `8389153592347c1534bcd60ab9a49e51f31881bcd176f49458d4106d41902fe7`
- sizing guard: 76631 / `191e3674cf22a75d4d519ab93d24fd00c7956bd55ebbd98c69cf185edb408553`
- supervisor: 250708 / `d600556c11a3d4c08709c9671e633cfab0bae8b995acecadec45831a532e39a7`
- sizing (topology): 239358 / `b50ee0bfa8ada4e997ab54d9fe8368976dea94e7ab027c47a9851049e46559a0`

### Complete suite results (44 runnable suites)

Every suite run as `timeout --signal=TERM --kill-after=5s 120s python3 -B tests/target_safety/<suite>.py`.
**All exit 0.**

| suite | exit | PASS lines | suite | exit | PASS lines |
|---|---|---|---|---|---|
| capacity | 0 | 10 | envelope | 0 | 5 |
| ceiling | 0 | 7 | namespace | 0 | 25 |
| ceiling_argv | 0 | 9 | receipt | 0 | 4 |
| ceiling_attempt | 0 | 5 | scenarios | 0 | 67 |
| ceiling_budget | 0 | 6 | sizing_admission | 0 | 10 |
| ceiling_bundle | 0 | 3 | sizing_bundle | 0 | 3 |
| ceiling_capacity_route | 0 | 5 | sizing_callback_timing | 0 | 4 |
| ceiling_capture_paths | 0 | 4 | sizing_collector | 0 | 7 |
| ceiling_defects | 0 | 6 | sizing_completion_integrity | 0 | 8 |
| ceiling_durable | 0 | 7 | sizing_failure_paths | 0 | 10 |
| ceiling_guard | 0 | 5 | sizing_outcomes | 0 | 7 |
| ceiling_main | 0 | 6 | sizing_reading_validation | 0 | 4 |
| ceiling_policy | 0 | 11 | sizing_resources | 0 | 5 |
| ceiling_production | 0 | 1 | sizing_route | 0 | 12 |
| ceiling_qa | 0 | 304 | sizing_rss_lifecycle | 0 | 10 |
| ceiling_stdin | 0 | 5 | sizing_selector_failures | 0 | 10 |
| ceiling_two_links_route | 0 | 5 | sizing_sqlite_absence | 0 | 12 |
| ceiling_window | 0 | 8 | sizing_sqlite_inotify | 0 | 4 |
| diag_frames | 0 | 3 | sizing_sqlite_route | 0 | 5 |
| diag_heartbeat | 0 | 4 | sizing_sqlite_stdin | 0 | 5 |
| diag_stream | 0 | 6 | sizing_sqlite_wire | 0 | 5 |
| diag_writer | 0 | 9 | window | 0 | 4 |

## Correction round 3 (final review: residual D2/D3/D5 code-level gaps)

Oracle final review (`task-5-hardening-corrections-final-review.json`) resolved
D1/D4/D6 and left code-level gaps in D2/D3/D5. This round closes all three and
adds a regression test for every Oracle counterexample.

### D2 (heartbeat phase cap, deadline recheck, shared diagnostic allowance)

- **Heartbeat cannot run past the collection phase** (`supervisor.py`): the
  collection heartbeat sink now writes with `until=guard.deadline` (the
  collection-phase safety deadline) and
  `bound=min(DIAGNOSTIC_ALLOWANCE_SECONDS, sample_interval)`. The Oracle repro
  (heartbeat write deadline 100.8 with collection ending at 100.0) is
  impossible: a write deadline is now `min(now + bound, guard.deadline)`.
- **Post-callback deadline recheck** (`collector.py`): after the synchronous
  heartbeat callback returns, the collector rechecks the collection deadline
  immediately, before processing any readiness event (loop-top discipline). An
  overrunning callback stops the pump with `collection_deadline` and never
  touches a pending readiness event.
- **Shared per-phase aggregate allowance** (`restoration.py`): the phase
  deadline algebra now carries a cumulative per-phase budget
  (`_PhaseAllowance`): every synchronous diagnostic write draws the allowance
  REMAINING for its safety phase and then accounts its elapsed wall time. The
  trio collection-completed + guard_result + cleanup_started shares ONE
  `DIAGNOSTIC_ALLOWANCE_SECONDS` budget before restoration - the former
  `min(phase_deadline, now + 1.0)` per write was a fresh second per write and
  could consume ~3 s collectively. Cumulative consumption is explicit in the
  allowance state.
- **Sampling-policy reconciliation** (implementation + claim sites): heartbeat
  writes are capped at one resource-sample interval, so the 0.1 s sampling
  cadence is delayed by at most that one interval (worst-case observed gap 2x
  nominal) and never by the 1.0 s allowance. `deadline.py`'s allowance comment
  and `resource.py`'s claim now state this reconciliation explicitly;
  `RESOURCE_POLICY`'s keys/value are unchanged (envelope mirror pinned).

### D3 (literal LF framing, stage/lifecycle order, unique entry, child chronology)

- **Framing on literal LF boundaries only** (`egress.lf_lines`, used by
  `report._single_outcome`, `report._terminal_complete`,
  `sizing_validation.parse_sizing_child`): `splitlines()` is no longer used for
  framing decisions on hardened streams - a CR-separated record never counts as
  framed (the CR blob fails to parse as one JSON record per LF line and is
  rejected). Legacy outcome-only semantics are preserved exactly, including the
  legacy ceiling guard-event parser in `report.py` (unchanged by design).
- **Stage order, overlap and lifecycle position** (`egress.diagnostic_stream_ok`):
  the stage brackets must appear as exactly `stage_started/completed` per
  expected phase in the expected sequence (no overlap, no interleaving, no
  extra or late brackets), and every bracket must close before the first
  lifecycle transition (`cleanup_started -> cleanup_completed -> egress_started`
  ordering itself was already enforced by `report._terminal_complete`).
- **Unique remote_entry**: exactly one entry record, first (the metadata record
  stays second, never later). A second `remote_entry` with a contiguous seq is
  now rejected.
- **Child chronology** (`sizing_records.child_chronology_ok`): hardened child
  transcripts must have readiness between the module_load and watch_session
  brackets and `topology_sizing_complete` LAST. Legacy child transcripts (no
  diagnostic record) keep their exact acceptance semantics.

### D5 (transport pump disposition is checked evidence)

- **Pump disposition slot** (`runner.PumpDisposition`): the pump thread reports
  `eof` / `deadline` / `error` (capture or journaling OSError) and
  `run_owned()` reads the slot only after a verified join. Only `eof` yields
  `RunResult.evidence_complete = True`; an injected `capture.frame()` OSError,
  the pump's own deadline or an unfinished thread make the transport result
  reflect failed evidence (the terminal lifecycle record carries
  `evidence_complete` and `pump_disposition`).
- **`_join_pump()` verifies the thread stopped**: an unfinished pump is
  incomplete evidence and is never accepted.
- **Unterminated tail is length-bounded**: the pump counts the full length of
  an LF-less tail while retaining at most `TAIL_RETAIN_BYTES` (4096) in
  memory; the `partial_tail` receipt carries the full counted length and the
  file-backed capture keeps every byte.
- **Route gate** (`stage1`): an incomplete-evidence transport is rejected even
  when its stdout carries an accepting outcome; the receipt records
  `remote.evidence_complete`. Concurrent pump start before `communicate()`,
  file-backed capture, kill-group semantics and owned-timeout behavior are
  preserved (ceiling_guard/ceiling_durable/diag_frames all green).

### New regression vectors (Oracle counterexamples -> now REJECTED)

| counterexample (previously ACCEPTED) | locked by |
|---|---|
| reversed stage pairs (seqs contiguous) | `diag_stream.round3_stage_order_and_entry_uniqueness_rejected` |
| cleanup/egress transitions before all stage pairs | same |
| second `remote_entry` with contiguous seq | same |
| CR separator instead of LF (supervisor and child streams) | `diag_stream.round3_cr_separator_never_frames_a_record` |
| `topology_sizing_complete` before module loading | `diag_stream.round3_child_completion_chronology_enforced` |
| completion not last (mid-transcript) | same |
| heartbeat write deadline past the collection end | `diag_heartbeat.heartbeat_writes_stay_inside_the_collection_phase` |
| heartbeat overrun then readiness processing | `diag_heartbeat.heartbeat_overrun_rechecks_the_deadline_before_events` |
| trio consuming a fresh second each | `diag_heartbeat.phase_diagnostic_allowance_is_shared_and_cumulative` |
| journaling OSError yielding exit-0 intact-looking evidence | `diag_frames.failed_journaling_is_a_visible_transport_failure` |
| unfinished pump accepted as complete | `diag_frames.unfinished_pump_marks_evidence_incomplete` |
| unterminated tail retained unbounded | `diag_frames.unterminated_tail_counts_length_beyond_the_retained_cap` |
| incomplete evidence accepted by the route gate | `ceiling_attempt.failed_evidence_transport_is_rejected` |

Already-rejected forms stay rejected (duplicate terminal, missing entry,
missing final LF, no brackets, metadata fourth, torn tail, sequence gap).

### Startup-violation qualification (explicit)

`startup_bound <= 20 s` is a **conditional assumption**, not an outcome: it is
measured as a conservative upper bound with ONE local monotonic clock - the
pre-GNU-launch timestamp to the receipt of a post-deadline-arming boundary
(`setting_read` stage start, **not** `remote_entry`) - and includes SSH
transmission delay. A violation (`> 20 s`) rejects the run on that
measurement alone. Never subtract remote and local monotonic clocks. **A
startup violation does not necessarily produce missing terminal output**: an
early-finishing run can still complete with a full, ordered terminal record
set; the startup violation and terminal completeness are independent
dispositions and either one alone rejects acceptance.

### File checksums and LOC (Round 2 -> Round 3)

| file | before sha256 | after sha256 | pure LOC |
|---|---|---|---|
| tools/target_safety/collector.py | `6c2c0d0aee83809b3de50d1e11f3fb9cec187e19aec8ca049ade881a17c50eb8` | `540009e0f776711f9163c767aebaa0aeae543e4858b68648e3043e23452c3b30` | 237 -> 242 |
| tools/target_safety/deadline.py | `8b3fe26f523369f7191b69fc23e3ff9f5b4572fa1a31417890fa028d8073bdf3` | `aae190f5449c46957f1d90dab0de7f9a9a0ef404f07a5dd780eca4912b8e57fe` | 42 -> 43 |
| tools/target_safety/egress.py | `d2762069ea65fc3c1f2f41a108319c85809220f1587e55507b47a00ea47ff805` | `316c7da6d8c0af614670b5bb9c8ee3661a9cdcf41a9fe8e2c1a886cf7acc3421` | 223 -> 245 |
| tools/target_safety/local_capture.py | `c1271c477f971a2cf8d5116457a089f805ee2ef50c388ef51747a346e98ff5c5` | `1b9eef7ef3406d3c382d935a432a2be6325d849ee50d4dc2cb352e588ab58b74` | 83 -> 85 |
| tools/target_safety/payload.py | `b85e6872d51e14eaa6770100d4832104fa26542ba1b67f4358865f194c3f1d83` | `0735a9f5a7f8467f75218b7fd214a9f2e61367ee2444e1a87fe175caae97ae50` | 61 -> 62 |
| tools/target_safety/report.py | `c68f4bb9e4c1b0fc974b6b98e10ff12d2dd803a64ba63c1d80e8086a79379d56` | `f2d782c06fd1f2a3a4a595deaa432261d0c3675ff9bc67207655f52666453db9` | 239 -> 238 |
| tools/target_safety/resource.py | `a5e443548e3d1710a14016726f9534c6864ce4fc9bf901b54a55fd473db1ad14` | `64c162d0e6dbee948447ed8bb3ba3a35d85c3e11b8c535557d92bb9e0da52be6` | 196 -> 196 (comment only) |
| tools/target_safety/restoration.py | `411b855fd2c006d00b14ab8de8e981912d2948a736c2fdc51a3bb776b9a2642f` | `0b29f937a9a7024bdf031e174604a12d02ff08320dc339222abc0abd52a6acef` | 90 -> 127 |
| tools/target_safety/runner.py | `df76905ca0ca223284ebd074c6d8a3a3c190f4d05f40dc946d055118665ef560` | `5ddce06ee0691cce5602c20c1e3e776448f1602267a972e783ca2e0fb999d7a1` | 154 -> 204 |
| tools/target_safety/sizing_validation.py | `1c67cfc12f0e6b9fda7e36c98f39a2cfe94617c04b7d5e5304c0e93855c344a8` | `5e7eb84dfdeaebed52f65a82ffd36ed809b3eb0eb7316d2457295858fc3d9509` | 248 -> 224 |
| tools/target_safety/stage1.py | `7fc013b62645d5252b2593fb422b61efcf97301d5cf91fdb1287711711536743` | `8367a4e7118a8292819535b6d4984aff9c1baa8b3d6db4e20f3df1cf0f9e4704` | 249 -> 249 |
| tools/target_safety/supervisor.py | `38631c52f94a8dd3cb1bd11cd32bab360a73a93a5c566c9eeba154c84c805ae6` | `d2ca6da304d3f68df43371c3ea808bb73e084a594d0d222a26a4a894b5347f15` | 249 -> 249 |
| tests/target_safety/ceiling_attempt.py | `d6d8dc79221bc9fa2fd4e830024e4d741049acd026a699e77e5cbcb30bf78d14` | `b14b9f7cef35c9a2b2530a53cf1d5ed4f9778409704dc45fde7d78bf5dcf899c` | 146 |
| tests/target_safety/ceiling_defects.py | `e809e0ffb5047d344b0c582ed5736f3a98b09ad7b6d921e077a50adb5a37c823` | `c39d9217afecf6692fe7637cd247fe1ada9905130af6e53e43ab240eac131506` | 169 |
| tests/target_safety/ceiling_policy.py | `b4cbbf357abe3a1ef9c98e660af74743d5a763bf36f5be4d80af195c9f9cb974` | `2e51124dbb8c4c1eba97122688612f855d0cfdfecac5688feae066f214b7143b` | 218 |
| tests/target_safety/diag_frames.py | `7ef9f61bff47d59bb2ea28a09b83ef3ffd02b47da125facd9abc378897e67716` | `8eaa30bf536bda42a6198744dee8d496e196abe269fd40becff772a70d1e6fac` | 83 -> 151 |
| tests/target_safety/diag_heartbeat.py | `64e0a03c595da9a0450e96bbc20dd301618f65f66eaee74fde06f24bb9471dfe` | `ee0117f7e4f18f18b2f1fc19290e27562622267a624b61586bcd2ad6d8ab2a29` | 117 -> 200 |
| tests/target_safety/diag_stream.py | `62c952d686a68d38dac6e837bb1ade6b9c6f7f826cffe58fc6ebd06fa90e9f06` | `d4a6967fc211a6db96eda623939b28ca9e02e431fd42da4ad41805e61274729f` | 169 -> 238 |

### New file (Round 3)

| file | sha256 | pure LOC | purpose |
|---|---|---|---|
| tools/target_safety/sizing_records.py | `157069a6e767f05c987d4e6133a9ce55e00fe3665c3cf1b19680c437da263a28` | 59 | typed-record discipline + hardened child chronology (extracted because `sizing_validation.py` sat one line under the enforced 250 pure-LOC gate; `SUPERVISOR_MODULES` lists it before its consumer) |

### Recomputed source vectors

Binding method: `sha256("\n".join(f"{path}:{sha256}" lines))` with `tests/`
sorted then `tools/` sorted (no trailing newline). The pre-round-3 state was
verified to reproduce the reviewed artifact vector exactly.

- **69-source set (binding file list, recomputed)**:
  `9c8b690a3f5ac10b204a62960bda9ab4909445bc4099bc4d8de5b9e8c04afc9b`
  (Round 2 / reviewed artifact: `ee4686b294146a3bdbcd6f3af147b4226e2d329f0af6aa01c964e35009fc759e`,
   Round 1: `8bbecbde6e25208312fcfc375115125376d5bc44bf468649c5f90cf0cb615782`,
   original: `31e658c1cf4ddc1c4cacd886f2385aba1fa909397d38d123d470a0df5a80f933`)
- **Full current set incl. sizing_records.py (75 sources)**:
  `ad9a10c9b82ecf834592f86f498978250165573fc4bd9a20205447b30b4109d3`
  (Round 2: `7d51af1c60c4de776a7094112f558e573f8d8872b465af886f86dcac7ff1598d`)

Production bundles (bytes / sha256):

- default guard `bundle()`: 90055 / `b088a1a1d46bd32ebd8cd4ed349e125f7c9006bad0098a69eb5381da74ea29a6`
- sizing guard: 78941 / `279fd9e05016ed2255c8546191c51759a8a480a96c418b08e1251a91c3d67784`
- supervisor: 260405 / `eb5b84e122ee37b6757a352ee0e38c3612c341fbf90d12a1b3f84384a295f840`
- sizing (topology): 249055 / `a7ef406a28c6767283a48f1d97ce931c4015c19a7161e3a8268d15575ee948f3`

### Complete suite results (44 runnable suites, final artifact)

Every suite run as `timeout --signal=TERM --kill-after=5s 120s python3 -B
tests/target_safety/<suite>.py`; `ceiling_capture.py` is a CLI tool (argv) and
`ceiling_fixtures.py` / `ceiling_setting_fixtures.py` / `sizing_sqlite_fixtures.py`
are import-only helpers - those four are not run standalone. **All 44 exit 0.**

| suite | exit | PASS lines | suite | exit | PASS lines |
|---|---|---|---|---|---|
| capacity | 0 | 10 | envelope | 0 | 5 |
| ceiling | 0 | 7 | namespace | 0 | 25 |
| ceiling_argv | 0 | 9 | receipt | 0 | 4 |
| ceiling_attempt | 0 | 6 | scenarios | 0 | 67 |
| ceiling_budget | 0 | 6 | sizing_admission | 0 | 10 |
| ceiling_bundle | 0 | 3 | sizing_bundle | 0 | 3 |
| ceiling_capacity_route | 0 | 5 | sizing_callback_timing | 0 | 4 |
| ceiling_capture_paths | 0 | 4 | sizing_collector | 0 | 7 |
| ceiling_defects | 0 | 6 | sizing_completion_integrity | 0 | 8 |
| ceiling_durable | 0 | 7 | sizing_failure_paths | 0 | 10 |
| ceiling_guard | 0 | 5 | sizing_outcomes | 0 | 7 |
| ceiling_main | 0 | 6 | sizing_reading_validation | 0 | 4 |
| ceiling_policy | 0 | 11 | sizing_resources | 0 | 5 |
| ceiling_production | 0 | 1 | sizing_route | 0 | 12 |
| ceiling_qa | 0 | 314 | sizing_rss_lifecycle | 0 | 10 |
| ceiling_stdin | 0 | 5 | sizing_selector_failures | 0 | 10 |
| ceiling_two_links_route | 0 | 5 | sizing_sqlite_absence | 0 | 12 |
| ceiling_window | 0 | 8 | sizing_sqlite_inotify | 0 | 4 |
| diag_frames | 0 | 6 | sizing_sqlite_route | 0 | 5 |
| diag_heartbeat | 0 | 7 | sizing_sqlite_stdin | 0 | 5 |
| diag_stream | 0 | 9 | sizing_sqlite_wire | 0 | 5 |
| diag_writer | 0 | 9 | window | 0 | 4 |

### Limitations update (Round 3)

- **Sampling cadence claim**: `supervisor_sample_interval_max_seconds=0.1` is
  the enforced scheduling interval; a heartbeat write is capped at one interval
  so at most one sample slips and by at most one interval (worst-case observed
  gap 2x nominal). It is not a hard real-time guarantee against scheduler or
  `/proc` reading cost. The policy dict shape is unchanged (envelope mirror
  pinned to the exact key set).
- **Legacy framing exception**: the legacy ceiling guard-event parser in
  `report.py` intentionally keeps `splitlines()` semantics - legacy
  outcome-only preservation is exact; only hardened stream framing is
  literal-LF.
- **Module size gate**: `qualification.source_checks` enforces <250 pure LOC on
  every tools/ and tests/ module. `sizing_records.py` exists because
  `sizing_validation.py` was one line under that gate; `supervisor.py` and
  `stage1.py` sit at 249 via formatting-only reflows. No semantic change was
  traded for line count.
- **Pump `deadline` disposition**: an escaped out-of-group descendant holding
  the transport pipe past the pump bound yields `evidence_complete=False` and
  an incomplete capture - the run is rejected (previously it looked like a
  clean transport). No whole-tree guarantee beyond the process group.
- **Type checking**: basedpyright was unavailable (explicitly declined); the
  enforced gates that did run are `qualification.source_checks` (AST parse +
  250 gate) and the full 44-suite sweep. No target contact, no network, no
  git, no kernel mutation in this round.
- **Requalification**: bound requalification must run against THIS final
  artifact (task5_completed stays False; sizing remains topology-only).
