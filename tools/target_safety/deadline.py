"""Authoritative remote deadline envelope: one allocation inside the GNU timeout.

The GNU client timeout (stage1.SSH_TIMEOUT_SECONDS, 960 s) starts LOCALLY,
before remote execution: its clock includes local spawn, the SSH TCP connect
and auth, the remote shell, `python3 -B -` interpreter start, stdin ingestion of
the whole bundle and the in-memory module load. That startup allowance is hence
explicit below instead of being an undocumented gap. The outer envelope is
preserved: GNU 960 is never extended; every budget is allocated inside it.

    startup_bound               20.0 s   local GNU start -> supervisor deadline armed
    collection_bound           900.0 s   == kernel.WINDOW_SECONDS (guard Watch window)
    termination_reap_bound      30.0 s   guard TERM/KILL/reap <= 15 + restoration <= 15
    egress_bound                 5.0 s   terminal result delivered before the slack
    scheduling_slack             5.0 s   kernel/SSH jitter before the channel dies
    ----------------------------------------------------------------------
    total                      960.0 s   <= GNU timeout (equality; slack is the margin)

The former 940-supervisor vs 900-guard relationship resolves exactly as
SUPERVISOR_BUDGET = collection_bound + termination_reap_bound + egress_bound +
scheduling_slack = 940: the guard keeps its full 900 s window, and the 40 s
cleanup headroom decomposes into reap/restore 30 + egress 5 + slack 5. Inside
the budget the existing formulas already place the phases: the guard cleanup
deadline sits 25 s before budget end (terminate/reap phase 900..915 =
collection end .. budget-25), restoration runs 915..930 (restore deadline
budget-10), egress runs 930..935 (terminal deadline budget-5) and slack runs
935..940. Startup 20 + budget 940 = 960: egress provably completes before the
worst-case GNU kill instead of racing it.

Numbers here are single-source: stage1.EFFECTIVE_CAPS mirrors them and the
envelope tests cross-check both sides so drift cannot pass qualification. The
diagnostic allowance above is carved from these phases (it is an allowance for
writes inside a phase, not a new envelope term).
"""
from __future__ import annotations

from typing import Final

from .kernel import WINDOW_SECONDS

# Local GNU start -> the remote supervisor arms its monotonic deadline: SSH
# connect (ConnectTimeout 10 s, one attempt) plus remote shell, interpreter
# start, 193885-byte stdin ingestion and module load. Healthy-path receipts
# show ~0.83 s; 20 s is the documented worst-case allowance inside GNU 960.
STARTUP_BOUND_SECONDS: Final = 20.0
# The guard Watch window is the collection bound; the supervisor child budget
# equals it minus any pre-collection setup time already spent.
COLLECTION_BOUND_SECONDS: Final = float(WINDOW_SECONDS)
# Post-collection cleanup: guard termination/reap is internally capped at 10 s
# (GUARD_TERM_GRACE + GUARD_KILL_GRACE) inside a 15 s phase, and restoration is
# three setting operations of at most 5 s each.
TERMINATION_REAP_BOUND_SECONDS: Final = 30.0
# Terminal-result delivery window; a record that cannot be delivered inside it
# fails closed as egress_failed_or_budget_exhausted.
EGRESS_BOUND_SECONDS: Final = 5.0
# Unallocated margin between completed egress and the worst-case GNU kill.
SCHEDULING_SLACK_SECONDS: Final = 5.0
# Explicit aggregate diagnostic allowance: the wall time that the synchronous
# diagnostic writes of ONE safety phase (stage brackets, lifecycle transitions,
# guard_result and readiness records) may take from that phase. The allowance is
# shared per phase and cumulative (restoration._diagnostic accounts each write's
# elapsed wall time), so collection-completed + guard_result + cleanup_started
# collectively can never consume more than this before restoration - not a
# fresh per-write second each. Per-write bounds derive from the enclosing phase
# deadline (the next safety deadline) and the remaining phase allowance - never
# from the whole remaining stream window - so diagnostics cannot eat reserved
# reap, restoration or egress time. Carved from the existing phase windows,
# never added to the 960 s envelope. Collection heartbeats are outside this
# aggregate and capped per write at one resource-sample interval (supervisor:
# bound=min(allowance, sample interval), until=collection deadline), so the
# 0.1 s sampling cadence is delayed by at most that one interval. The terminal
# result is not a diagnostic write: it keeps the 5 s egress bound
# (egress.Writer.terminal).
DIAGNOSTIC_ALLOWANCE_SECONDS: Final = 1.0
# Remote-side budget from deadline arming to budget end.
SUPERVISOR_BUDGET_SECONDS: Final = (COLLECTION_BOUND_SECONDS + TERMINATION_REAP_BOUND_SECONDS
                                    + EGRESS_BOUND_SECONDS + SCHEDULING_SLACK_SECONDS)
# Everything the supervisor reserves after the collection window.
CLEANUP_HEADROOM_SECONDS: Final = (TERMINATION_REAP_BOUND_SECONDS + EGRESS_BOUND_SECONDS
                                   + SCHEDULING_SLACK_SECONDS)
# The full envelope as the local GNU timeout sees it (never extended).
GNU_TIMEOUT_SECONDS: Final = STARTUP_BOUND_SECONDS + SUPERVISOR_BUDGET_SECONDS
