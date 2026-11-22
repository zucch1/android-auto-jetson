# Task38 wireless reconnect and recovery evidence

Status: **real-device qualification pending**. Historical host evidence remains
host-only, not production wireless admission or full wireless compliance. The
authenticated type5 bond has survived reconnect, but HFP presence still fails;
there is no passing real-phone baseline or full-scope qualification receipt.
The schema support below does not change those outcomes. Parent-supplied actual
target receipts and explicit owner sign-off are still required for closure.

## HFP link (operator step, #160)

Wireless admission needs a Bluetooth HFP link between the phone and the Jetson:
only then does the phone's Android Auto dial the AA RFCOMM profile. The
infotainment system owns that link; the helper and receiver never create it. Until
infotainment (or a future connect service) brings it up, a wireless test window
must bring HFP up explicitly, after the receiver is running and before the AP
handoff is expected:

```sh
tools/wireless-qa/aa-hfp-link-test-only.sh connect "<phone Bluetooth alias>"
# ... window ...
tools/wireless-qa/aa-hfp-link-test-only.sh disconnect "<phone Bluetooth alias>"
```

The tool exits 0 only after it has confirmed the result in WirePlumber's bluez5
device profile (`headset-head-unit*`); `Device1.Connected` alone is not HFP (BlueZ
5.72 may satisfy `ConnectProfile` with an LE ATT link on a dual-mode phone). A
failed or unverified connect or disconnect exits 1.

Without it, every attempt expires as
`wireless_admission_expired` with `reason` `no_bluetooth_link` ("waiting for HFP
(owned by infotainment)"). Also unplug the phone's USB cable from the Jetson
first: Android Auto suppresses wireless setup while it believes a USB head unit
is attached ("Ignoring wireless setup event because of USB accessory device
attached", window 3, 2026-11-18).

## Wireless audio: credit window + playout (owner decision 2026-11-20)

`aa-wireless-receiver` shares `AudioRenderWorker` and `pcm_router_config()` with
the wired receiver (`live_pcm_drift.enabled = true`, bounded 5 ms head trim
above 20 ms), but schedules wireless audio differently.

Why: the A26 sent media audio stop-and-wait (`maxUnackedFramesAllowed: 1` in
its log), one 2048-frame (42.67 ms) chunk per credit round trip, and the
receiver placed each chunk at its arrival stamp. Any round trip over one chunk
period then became a gap (silence) followed by a burst (trimmed, or judged
late). Window 9 (#180, TCP_NODELAY) still trimmed 5.8% and filled 8.4% with
silence over 26 s of audio, then ended `late` (~218 ms) right after a Wi-Fi
spike second of 12 acks at 83 ms.

Policy (wired is unchanged: stop-and-wait and the arrival model with the
#155 90 ms catch-up):

- **Credit window** (`--audio-max-unacked`, default 4): every audio channel
  advertises `max_unacked` 4, so the phone may keep four chunks
  unacknowledged. 4 x 42.67 ms = 170 ms of credit, more than a 150 ms round
  trip and twice window 9's 83 ms spike second, so a spike delays arrivals by
  its one-way time instead of stalling the phone (window 10: the A26 logged
  `maxUnackedFramesAllowed: 4`). The render worker reserves all N permits the
  phone holds at every channel reset and bypass exit, and scales each role's
  slots and byte reservation by the window, so a chunk the phone may legally
  send is never refused while the sink blocks; `AvSink` holds up to that many
  pending deliveries and acks each one as it completes. The A26 honored the
  window in live testing. A delivery beyond the advertised window is refused
  because the worker reserves capacity for exactly N. Stop-and-wait (N = 1,
  wired) keeps its historical accounting.
- **Playout clock** (`--audio-playout-ms`, default 120, max 180): each role's
  stream is anchored 120 ms after the arrival of its first chunk, and every
  later chunk plays back to back after the previous one. Bursts and droughts
  inside the buffer therefore cost no trim and no silence. 120 ms covers a
  one-way delay excursion of 120 ms (a round trip near 250 ms on a 10 ms link)
  and leaves room for a 42.67 ms chunk in the 200 ms PulseAudio ring when
  deliveries are evenly spaced. At 180 ms, even one full chunk can exceed the
  ring. Grouped deliveries can exceed it at 120 ms too (the offset reached
  308 ms in window 11). A full ring can block the worker and, once the
  reserved worker queue fills, withhold credits from the phone. The block
  duration was not measured in that window.
- **Underrun**: a chunk arriving after its slot re-anchors the role 120 ms
  after that arrival. The elapsed hole is skipped without a write
  (`frames_skipped`), the refill is at most 120 ms of silence
  (`frames_silence`), `audio_playout_underrun` is logged, and the session
  continues. Playout never fails `late`. A stream silent more than 1 s past
  its slot (a pause without a stop message) re-anchors the same way but counts
  as a resume (`playout_anchors`, `playout_resumes`), not an underrun. Every
  anchor logs `audio_playout_anchor` with the elapsed time it skipped (each
  one, even several in one render step), so a large forward jump is always
  visible. An arrival clock that runs backwards by more than 200 ms fails
  closed `late`, as on the arrival model. The refill of a re-anchor is the
  buffer (at most the depth), not gap silence, and does not count against the
  200 ms aggregate silence cap, so two re-anchors in one render step (media
  and guidance) cannot end the session.
- **Drift**: the buffer depth of a chunk is its slot minus its arrival. The
  A26 delivers in groups (windows 10-12: `max_arrival_burst` 5). It captures
  in real time (`Start capturing system audio with sampling rate: 48000,
  buffer size: 65536, expected frame duration: 42.666667 ms`, an
  `AudioRecord`) and its acks stay at real-time rate, so a group's chunks
  were captured on time and delivered late, the last one on time. Single
  depths therefore spread by up to the group (window 11: p1 44, p50 156,
  p90 185 ms), and a late chunk lowers one; both drain by themselves.
  Correction judges the window's smallest depth (the group's first,
  latest-delivered chunk) once the last 4 s hold at least 2 s of arrivals:
  - a chunk loses the bounded drift trim (at most 5 ms) only when it is more
    than 20 ms above 120 ms (phone clock faster, a backlog after an
    underrun);
  - at most 5 ms of silence is inserted (`frames_padded`), refilling toward
    half the depth (60 ms), only when it is below 30 ms (phone clock slower,
    a slower path), so a jitter spike inside the buffer costs nothing. After
    a sustained slower path the buffer therefore settles near 60 ms, not
    120 ms: underruns after such a step are expected sooner (R6);
  - within 4 s of an underrun the window is the last 0.5 s (older entries
    are dropped and the re-anchor group itself, every chunk within 10 ms of
    it, is ignored) and 0.25 s sustains, so the backlog that follows starts
    draining at once;
  - while the offset (the window's largest depth, the value the video hold
    is capped against) is above min(depth + 230 ms, 350 ms) and the buffer is
    over-full (after an underrun: the current chunk; otherwise the window's
    floor), the backlog is shed at 30% of each chunk (`frames_shed`). 30% is
    the most the reported offset's slew can follow, so the offset the video
    follows stays the delay audio plays with. It is audible: a 12.8 ms hole
    in every 42.67 ms chunk, a ~23 Hz gating, for the fraction of a second
    it runs right after an underrun. With the A26's 341 ms AudioRecord buffer
    the post-drought offset peaks at most 60 ms above the 400 ms cap, for at
    most 0.9 s (the replay tests' bound, with the phone grouping 4 or 5
    chunks); while it is above, video plays early by that excess. The shed
    target is absolute (350 ms), which is why `--audio-playout-ms` stops at
    180: a deeper buffer would be drained to it. `playout_shed_residual_chunks`
    counts chunks whose committed offset still exceeds the shed target,
    including when the buffer minimum prevents further shedding.
    `playout_shed_residual_peak_ms` is the largest remaining excess.

  Window 10 (8fe9ea5, 244 s, passed) showed why: the first rule took the
  largest depth over 4 s as the reference, so every group read as excess
  latency and was trimmed (0.81%) until the buffer was nearly empty
  (`playout_depth_min_ms` 1.0); ordinary 3-28 ms jitter then underran it
  13 times. The replay tests release chunks in groups of 2-5 and expect no
  trim, silence or underrun, and pad tests run with groups of 3 and 4.
- **Catch-up is not used for wireless.** `--audio-catch-up-watermark-ms`
  is accepted only with `--audio-playout-ms 0`, which restores the arrival
  model (with `--audio-max-unacked 1`, exactly #180's behaviour). Playout
  also requires the drift policy itself (`live_pcm_drift.enabled`); the
  router refuses to open without it.
- **Video** (`--video-delay-ms`, default `match`): the receiver holds every
  video access unit, still stamped with its arrival time, for **the playout
  offset `audio_write_completed` reported for that frame's own timestamp**:
  the delay the audio it pairs with really plays with. The hold is decided
  when the frame is checked for release, from the writes made by then, and
  it never falls faster than half the arrival spacing below the previous
  frame's, so a frame is never pinned behind another. Across a gap in the
  audio (an underrun, a short pause) the lookup follows the router's own
  model: the line between the offsets either side, and, while the gap is
  still open, a fall of a third of the content time past the last write,
  down to the depth. That is the router's clamp: it may lower the offset by
  half the output gap at once (its times are output-clock times), and with
  the drop d and content gap cg = og + d, d <= og/2 is d <= cg/3. (Round 5
  briefly used half the content time, a misreading that released frames in
  an open gap up to ~90 ms early; round 6 reverted it.) Until the first
  playout audio write, and after a second without audio (a pause without a
  stop message, including one already recorded: video latency then drops to
  the depth and climbs back on resume, with no audio to pair), it holds for
  the 120 ms depth. Above 400 ms it is capped, and every capped frame is counted
  and logged (`video_hold_capped`). Held frames are released at their due
  by a timer thread of the outlet's own (it sleeps until the front frame's
  due, re-checking every 5 ms since a new audio write can move a due
  earlier), and also from the runtime's checkpoint, even while the session
  is mid-send (but not during an IDR request's own send: the frames held
  then are older than the keyframe asked for); an IDR request either
  release triggers waits for the session and is dropped if the keyframe
  arrives first. A frame released more than 20 ms after its due is counted
  and logged (`video_release_late`, with `by`: timer, checkpoint, arrival or
  service, and `checkpoint_gap_ms`: how long the owner's checkpoint had not
  run). `--video-delay-ms N` fixes
  the hold at N ms instead (it needs `--audio-playout-ms` > 0).

  Why the hold follows the offset (window 11, N1): with grouped delivery the
  audio anchored on a late chunk plays depth + (group - 1) chunk periods
  after on-time content, not 120 ms. Window 11 (e1e70b1, 480 s) reported a
  playout offset of p50 167 ms (p90 205, p99 295) against a fixed 120 ms
  video hold. Its A/V pairs, through the drive smoke's own pairing and the
  signed contract, **failed**: p50 -24 ms, worst 5 s mean -150 ms, 680 pairs
  over 90 ms. Holding audio to a fixed 120 ms instead is what window 10
  effectively did (offset ~136 ms): on this link it underran 13 times in
  4 minutes.

  Window 12 (ed60f1a, 485 s, the hold following the offset read at frame
  arrival): p1/p50/p99 +11.8/+19.1/+45.3 ms, no pair over 150 ms, but the
  signed contract **failed**: 2 pairs over 90 ms and a worst 5 s mean of
  92 ms. Both had a cause outside steady state:
  - Startup (worst 5 s mean, the +99 ms pair at 5.8 s): audio starts 5.5 s
    after the first paint; the anchor reports 120 ms, and the phone's
    opening backlog takes the offset to ~300 ms within 0.5 s. The hold was
    read from the newest write, which pulsesink accepts up to ~200 ms
    ahead, so the first paired frames were held for later audio's offset.
    The hold is now read for the frame's own content time.
  - 349.5 s (+103 ms): the offset was a steady 189.1 ms; the receiver sent
    frames 10310-10312 over IPC in the same 0.03 ms, 278/243/198 ms after
    their arrival. No video arrived for the same 110 ms: a Wi-Fi stall
    blocked an outbound session send, and the runtime skips its guarded
    video service while one is in progress. Held frames are now released
    without session I/O.
  Window 13 (83ab293, 485 s): the startup transient is gone (p1/p50/p99
  +11.9/+19.0/+42.2 ms, worst 5 s mean 24.8 ms), but 3 pairs over 90 ms
  (95.8, 97.5, 95.8 ms at 228.7, 267.8, 365.0 s) still **failed** the
  `cap` clause. All three are release stalls, not offset errors: the audio
  offset is steady (179.1 / 183.9 / 178.0 ms) and a batch of frames left
  together 80-85 ms after their due (frames 7966-7971 at 267.798 s, held
  250-268 ms, with nothing sent for 217 ms before them). The display was
  idle (each frame received within ~1 ms of its send) and the audio worker
  kept writing, so the receiver's session thread did not release for that
  long, each time right after a burst of post-stall arrivals. The third
  coincides with an underrun. The likely path is the render worker's lock:
  the hold lookup and the session's audio submit both took it while the
  worker decoded the burst through a GStreamer round trip. The lookup now
  has its own lock, playout media PCM already in the output format skips
  the round trip, and late releases are logged so the next window shows
  whether any remain. Replaying window 13's records through this round's
  logic (releases at their due): max 44.6 ms, p99 35.5 ms, worst 5 s mean
  22.3 ms, no pair over 90 ms; `cap` and `soak_rate` pass.

  Window 14 (795cf2d, 480 s): p50 +19.1 ms, p99 +43.9 ms, `window_mean`
  and `p99` pass, but 4 pairs over 90 ms (102.7 and 103.8 ms at 132.33 s,
  92.3 and 96.4 ms at 247.55-247.57 s) **failed** `max_abs`, and
  `video_release_late` fired ~270 times (late_ms mostly 26-57). The lock
  change did not remove the cause because the cause is not a lock: the
  owner (session) thread itself stops running. Release lateness, from the
  receiver's own records (each frame's `video_ipc_send` against arrival +
  the hold of the audio it pairs with), is p50 1.9 ms and p90 5.1 ms (the
  5 ms checkpoint cadence while TCP is idle works), but 297 frames left
  more than 20 ms late, in 178 episodes spaced a median 2.0 s apart. In
  those episodes the session thread stamped no arrival at all: 2 video
  arrivals fell between the late frames' due and their release, against
  197 in random windows of the same lengths, and 256 arrived in the 10 ms
  after the release against 42 at random. The data was waiting in the
  socket, and was read the moment the thread ran again. The render worker
  kept writing audio during half of them. Which call blocks the owner
  thread is not visible in the records. Both excursions are such episodes:
  frames 3891-3894 left 79-91 ms late at 132.35 s, inside the Wi-Fi stall
  that caused the second underrun (the offset was a steady 177.8 ms until
  that underrun); frames 7319-7321 left 59-73 ms late at 247.56 s with the
  offset a steady 181.7 ms and no underrun. Held frames are now released
  by the outlet's own timer thread, which does not depend on the owner
  thread (the outlet test releases 40 grouped frames with no checkpoint at
  all: lateness p50 0.5 ms, p90 0.6 ms, max 0.6 ms), and `video_release_late` now
  says which path released the frame and how long the checkpoint had not
  run. Replaying window 14's records with releases at their due: max
  50.3 ms, p99 35.8 ms, worst 5 s mean 23.2 ms, no pair over 90 ms; `cap`
  and `soak_rate` pass.

  Replaying window 12's own records through the new outlet logic (hold for
  the frame's content time, decided at release, slewed, capped; display time
  after release as measured): p1/p50/p99 +11.3/+16.3/+35.5 ms, max 84 ms,
  worst 5 s mean 24.5 ms, 0 pairs over 90 ms. The `cap` and `soak_rate`
  clauses both pass on that replay. It assumes frames leave at their due
  time; the live release path is what the next window must confirm.

Latency this adds over the wired arrival model: the playout offset, for audio
and for video alike (the hold follows it), which includes touch-to-photon on
the projected UI and every IDR recovery. It is the 120 ms buffer plus however
late this phone's groups and opening backlog make the anchor chunk: windows
11-12 settled at about 150-220 ms (window 12 ended at 217 ms), more for a
moment after a drought until the backlog is shed. **The owner accepted the
cost** when choosing "Credit window + playout, ~100-150 ms buffer, video
matched" on 2026-11-20. The buffer is still 120 ms; the rest is the A26's
grouped delivery, which a fixed 120 ms delay cannot ride out without frequent
underruns. Lip sync stays where it was.

IDR and recovery budgets with the hold: a recovery IDR reaches the display
one hold later, so each waiting_idr episode is that much longer. Against the
drive smoke's `waiting_idr_max` (1.0 s per episode) that leaves 880 ms at the
120 ms depth, ~780-830 ms at windows 11-12's 170-220 ms, and 600 ms at the
400 ms cap. The producer's IDR re-request interval (250 ms) may send one or
two extra requests while a recovery IDR is held, and the focus-transition
escalation (after three nudges or 1.0 s) has one hold less margin. Windows 10-12 had no
mid-session recovery episode to measure this (only startup episodes). The
next drive smoke must show recovery episodes under 1.0 s with the hold.
On wireless, video setup sends the initial focus notification. The receiver
requests an IDR at the first accepted video start for the stream, after any
staged codec-config ACK. This can precede the transition to active. With a
consumer attached, the receiver checks for a retry at its session checkpoints
using the 250 ms interval even if no frame arrives. The display's request
is accepted once the link is active. Measure the startup budget from the
active transition to the consumer's first recovery.
Frames still held when a backpressure overflow requests a recovery IDR, when
the consumer disconnects, or when a consumer is replaced, are dropped rather
than sent after it.

A/V gates: video is delayed rather than the metric relaxed. On the playout
clock `audio_write_completed.scheduled_ns` is the content's arrival-clock time
and `playout_offset_ns` the delay after it: the depth of the least-delayed
recent arrival, which marks on-time content. The reported offset moves toward
that estimate by at most half the output time that passes, in both
directions, so the reported content interval never starts before the
previous one ended (the collector drops a write whose schedule does not
advance), and the video hold that follows it only ever rises in arrival +
hold. `presented_ns - scheduled_ns` therefore includes the offset, and so
does the display's paint latency for the held video. The signed contract
(5 s mean within 40 ms, p99 50 ms, drive smoke max 90 ms, soak at most 1 pair
over 90 ms per 30 min and none over 150 ms) measures the same thing as on
wired and is not relaxed. `--video-delay-ms 0` keeps video at arrival; the
contract then sees the audio lag and fails, so the offset is never hidden.

The #60 stall bypass still starts 250 ms after a credit is first withheld.
With the window of 4 the worker withholds credit only once its scaled
reservation is full, about 500-680 ms of queued media, so on wireless the
bypass engages later than on wired (~128 ms of queued audio).

Telemetry for the next live window: `{"event":"wireless_media_policy",...}` at
startup (`video_hold` "match" or "fixed", and `video_delay_ms`, always a
number: the fixed hold, or the hold before the first playout write), and
`media_timing` carries `frames_trimmed`, `frames_silence`, `frames_skipped`,
`frames_padded`, `frames_shed`, `playout_depth_ms`, `playout_depth_min_ms`,
`playout_depth_max_ms`, `playout_offset_ms` (the router's target),
`playout_reported_offset_ms` (what the writes report and the hold follows),
`playout_underruns`, `playout_anchors`, `playout_resumes`, `credit_window`,
`max_unacked_held` (our side: acks go out on receipt, so normally 1) and
`max_arrival_burst` (the phone's side: chunks of one channel arriving within
10 ms of each other). The counters cover every sink route of the receiver's
life (`sink_reopens`). See `tools/perf/README.md`.

What a live window should check:

1. The phone log says `maxUnackedFramesAllowed: 4` for AUDIO_STREAM_MEDIA,
   and `max_arrival_burst` > 1 shows the phone using the window. No
   `MediaSourceFlowController out of permits` lines.
2. `CAR.AUDIO.STATS` ack latency stays well under 170 ms; acks/s stays near
   23.4 (real time) even through a spike second.
3. `frames_trimmed` and `frames_silence` stay near 0 (window 10: 0.81% and
   0.66% before the sustained-error rule; window 11: 0.14% and 0.10%;
   window 12: 0.25% and 0.22%), `playout_underruns` is 0 or rare
   (windows 11-12: 4-5 in 8 min), every `audio_playout_underrun` has
   `late_ms` matching a Wi-Fi spike, and `playout_depth_min_ms` stays well
   above 0. `playout_offset_ms` sits above 120 ms by about the phone's group
   span; that is the real delay, and the video hold follows
   `playout_reported_offset_ms`. No `video_hold_capped` outside a drought.
4. No `late` failure and no `audio_sink_failure`; the session outlasts the
   ~33.5 s of window 9.
5. The drive smoke's signed A/V criterion, judged on the run's own records
   with `aa_drive_smoke.av_pairs` and `aa_av_contract.evaluate`: it has not
   yet passed live (windows 11-14 failed it, see above). No
   `video_release_late` should appear; if one does, its `by` and
   `checkpoint_gap_ms` say whether the timer itself was late. Record its
   result as measured, including the first seconds after audio starts and
   any pair over 90 ms with its cause. Every waiting_idr episode should
   stay under 1.0 s with the hold.

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
