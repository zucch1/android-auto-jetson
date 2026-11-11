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
callbacks; `frames_delivered` is the cumulative delivery count.

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

The controlled-clock regression submits 90 correlations, injects a first-output
time of nine seconds, and checks both pump-entry expiry and a blocking pull
crossing the boundary. It requires an error and exact loss count, with no real
five-second sleep. Other tests cover the exact retention boundary, IDR
recovery, unchanged PTS identity, capacity, EOS, and notice persistence.

Jetson validation remains necessary: retain the original raw failures and
exact source/binary hashes, regenerate and retain the mapped-file manifest,
and record `fincore` immediately after a fresh build **before any pre-read**,
with per-file and byte-weighted residency. Independently test first decode
after boot or a controlled cold-page equivalent, recording delivered/lost
counts and errors for both automatic and software backends. Mostly resident
pages would contradict or limit the build-eviction attribution. A subsequent
warm-conditioned PASS alone does not establish that cause. Pre-reading is
validation conditioning, never a product correction or cold-start
qualification. Existing retention, latency, A/V and qualification gates stay
unchanged; this change makes no hardware-performance claim.
