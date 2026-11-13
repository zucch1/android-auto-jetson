# Consumer first-decode retention loss (#76)

The five-second correlation retention policy and 512-entry bound remain
unchanged. They are operational bounds, not decode latency qualification.
Expiry still invalidates the entire pending set and requires a newer IDR;
timestamps remain strictly increasing and outputs are never relabeled.

`DecodeHelper::pump()` now returns nonterminal `ipc_consumer_rejected` when
accepted AUs have been sacrificed by retention, capacity, or EOS. The notice
survives a recovery IDR accepted by `submit()` before the next pump. Multiple
loss episodes before a pump coalesce into one notice, while `DecodeReport`
retains cumulative AU counts separately as `frames_lost_retention`,
`frames_lost_capacity`, and `frames_lost_eos`. Late unmatched output does not
increment these counts again. `awaiting_idr` reports the current recovery
state. A caller can resume pumping and submit a newer IDR when required, or
end the session explicitly. A failing pump may already have delivered earlier
callbacks; `frames_delivered` is the cumulative delivery count. Clean EOS with
stranded accepted AUs intentionally produces the same one-shot nonterminal
notice; no further input or IDR recovery is expected on that ended stream.

The dummy display reads these live counters after every controller pump. Its
existing telemetry writer emits a `decode_loss` JSON line for each changed loss
reason (`expiry`, `capacity`, or `end_of_stream`) with the cumulative count and
IDR state. Repeated episodes within one second per reason coalesce into the
next line without losing the final total. The existing test-control `status`
response also carries all three cumulative counters and `awaiting_idr`; neither
the four-event `display_error` cap nor repeated-error suppression applies to
these loss lines.

## Preserved historical failure and attribution limits

[Issue #76](https://github.com/zucch1/android-auto-jetson/issues/76) records
source `8b1420a9e21fb84e5d15a0fe9e6a7d1ca843de46`: the fresh automatic-backend
fixture delivered **0/90**, while its software companion delivered **90/90**.
The diagnostic derivative invalidated all 90 correlations at **5.020625888 s**;
the first hardware sample arrived at **9.248719339 s**, retaining original
PTS **5000000000**. These historical FAIL results remain FAIL.

The owner-reported unchanged binary was named `singlejob-01`. Evicting about
119 mapped regular files / 497 MB produced roughly six-second runs and three
failures in four; pre-reading produced roughly 0.48-second runs and three
passes in three. `decoder:src` was observed in `folio_wait_bit_common`.
Dirty-page and anonymous-pressure controls alone did not reproduce this.
These are attributed reports, not measurements repeated by this change.
The supplied issue does not contain an exact binary hash or the original
mapped-file manifest; this document does not invent either identity.

## Validation and remaining native work

Through-pump tests (`ConsumerDecodeCorrelation.Pump*`) drive the real software
`DecodeHelper` with an injected clock; no real five-second sleep and no enlarged
retention is involved:

- `PumpBlockingPullCrossingRetentionReportsLoss`: the clock is inside the
  window at pump entry (4 s) and past it when the first blocking pull returns
  (9 s). Only the post-pull check can raise the loss; the test also proves the
  report reference taken before `pump()` is refreshed by it.
- `PumpEntryExpiryReportsLossBeforeAnyPull`: the other expiry path (loss seen at
  pump entry, no pull), plus IDR resync visible on the held report.
- `PumpPostPullDecoderErrorOutranksLossNotice`: a decoder error landing during
  the blocking pull is the result of that pump (terminal), not of the next one.
- `PumpEosStrandedAuReportsEndOfStreamLossOnce` and
  `PumpExpiryBeforeEosIsNotCountedAgainAsEosLoss`: EOS ordering. Stranded AUs
  are counted once, as `end_of_stream`, or as `expiry` when retention already
  claimed them (no double count).

**Clean EOS with stranded AUs fails one pump (decision).** Accepted AUs that
can never produce output are lost frames; returning success with zero delivered
would be the silent loss of #76. The notice is nonterminal and one-shot, so the
session still ends normally. A consumer that treats any pump error as fatal
should treat `ipc_consumer_rejected` after its own `end_of_stream()` as "stream
ended with N AUs undelivered" instead.

**Test seam.** The injected clock and `DecodeHelperTestAccess` exist only under
`AA_DECODE_HELPER_TEST_CLOCK`, defined solely for the `aareceiver-consumer-testclock`
library that `aa_consumer_decode_test` links. The production
`aareceiver-consumer` library is compiled from the same source without it: no
data member, no indirection, no symbol (checked with `nm`). The display and all
shipped targets link the production library. `DecodeTestAccess.hpp` refuses to
compile without the macro.

State-level tests cover the exact retention boundary, IDR recovery, unchanged
PTS identity, capacity/EOS accounting and notice persistence; display tests
cover reasoned, cumulative, rate-limited telemetry, counter reset between
decode sessions and the final flush, and the executable replay test checks the
status counters.

Measured on the Jetson (not a cold-page reproduction): a real `aa-dummy-display`
with the software backend run in a process-local `systemd-run --user --scope
-p CPUQuota=5%` (a slow decoder, no root) presented 63 of 90 frames and, on this
branch, reported `frames_lost_retention: 27`, one `display_error` and one
`decode_loss` line (`reason: expiry, count: 27`); the `main` binary under the
same quota presented 63 frames with no counter and no loss line. Runs are
timing dependent. Unprivileged per-file eviction (`posix_fadvise DONTNEED`) did
not remove any page of the 568 decoder/GStreamer/NVIDIA files tried (mapped by
running processes or otherwise pinned), so an actual cold page-in could not be
produced; after idle the first hardware-backend fixture run took 3.6-4.8 s
against 0.65 s warm, below the 5 s bound, with no loss.

Remaining Jetson validation: retain the original raw failures and exact
source/binary hashes, regenerate and retain the mapped-file manifest, and record
`fincore` immediately after a fresh build **before any pre-read**, with per-file
and byte-weighted residency (here every mapped decoder file was 100% resident
when sampled, which neither supports nor refutes the build-eviction attribution
for a fresh build). Independently test first decode after boot or a controlled
cold-page equivalent, recording delivered/lost counts and errors for both
automatic and software backends. A warm-conditioned PASS alone does not
establish that cause. Pre-reading is validation conditioning, never a product
correction or cold-start qualification. Existing retention, latency, A/V and
qualification gates stay unchanged; this change makes no hardware-performance
claim.
