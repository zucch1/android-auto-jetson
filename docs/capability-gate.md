# Capability gate report (task 13)

Machine-readable companion: [`docs/capability-gate.json`](capability-gate.json)
(schema `aa-capability-gate/1`). Checker: `tools/gates/check_capability.py`.
This report is self-contained and redacted: it carries real measured values,
evidence receipt names with SHA-256 digests, and exact owner sign-offs, but no
public identifiers, IP addresses, MAC addresses or serial numbers, and it has
no runtime dependence on the git-ignored `.omo/` evidence tree.

Run the gate:

```sh
python3 tools/gates/check_capability.py docs/capability-gate.json
```

Exit codes (see the plan "Budget and release authority" contract):

| exit | meaning |
|------|---------|
| 0 | every area is `full-pass` / `probe-pass-provisional` / `fallback-probe-pass` and every budget available at the run stage is measured and passing |
| 2 | continuation only: at least one `owner-signed-reduced-scope` area with a valid `owner_signoff` naming its `withdrawn_claims`; never counts as full compliance |
| 1 | blockers, missing areas, invalid sign-offs, weakened/redefined budgets, unmeasured budgets falsely claimed as passed, unsigned budget misses, or unresolved interim classes at `--stage final-release` |

Stage semantics: the default stage is the report's declared stage
(`continuation`). `--stage final-release` additionally requires every
`probe-pass-provisional` / `fallback-probe-pass` entry to have resolved to
`full-pass` or `owner-signed-reduced-scope`, and requires every release-stage
budget (audio, reconnect, recovery, soak, RSS) to be measured.

## Target and phone

| | |
|---|---|
| Target | aarch64, L4T R39.2.1, kernel `6.8.12-1021-tegra`, GStreamer 1.24.2 |
| Phone | Samsung Galaxy S24 Ultra, Android 16, Android Auto 17.7.663654-release (owner-reported; not read through ADB) |

## Area classifications (continuation stage)

| area | classification | headline result | evidence |
|------|----------------|-----------------|----------|
| dependencies | probe-pass-provisional | dependency lock check PASS (source-pins=3); immutable sysroot archive/manifest digests accepted (task-5 gate PASS) | `task-3-dependencies.json`, `task-5-final-acceptance-gate.json`, `task-5-sysroot.json` |
| target identity | probe-pass-provisional | aarch64 / L4T R39.2.1 / 6.8.12-1021-tegra / GStreamer 1.24.2 observed | `task-5-sysroot.json`, `task-9-decode-probe.json`, `task-11-usb-probe.json` |
| ABI | probe-pass-provisional | `aasdk-cpp20`; 4/4 ABI tests, nm/ldd-resolved boundary symbols, C++17 consumer rejection proven | `task-7-final-acceptance-gate.json` |
| TLS | probe-pass-provisional | `tls_posture` 32/32; encryption-only-compatibility default gated by approved-phone callbacks; verified-peer test-only rejects unknown credentials; no production key markers | `task-8-final-acceptance-gate.json` |
| decode | **owner-signed-reduced-scope** | 600 s hardware decode 18000/18000 frames, zero drops, 30 fps, 10.02 Mbps, 3.83% CPU; p95 36.09 ms **exceeds** the 33 ms budget (withdrawn); software fallback unqualified (withdrawn) | `task-9-decode-probe.json`, `task-9-owner-signoff.json` |
| IPC | probe-pass-provisional | host-only 600 s synthetic AF_UNIX benchmark: 12.189683 Mbps, p95 0.2 ms, zero happy-path drops | `task-10-final-acceptance-gate.json` |
| USB | probe-pass-provisional | AOA2 capability only: IN 0x81 / OUT 0x01, MPS 512, 3/3 resets, 30 s / 31 samples stable — **not projection** | `task-11-usb-probe.json` |
| wireless | **owner-signed-reduced-scope** | Regd fixed (shim+watchdog, US live); UNII-1 5 GHz AP (ch 44, 20 MHz) + client-association **demonstrated** (2026-10-10/11 receipts); claim 1 re-scoped from channel-36 per 2026-10-11 sign-off; both release budgets now measured: `ap_startup_s` **pass** (4.223 s) but `session_reconnect_s` **MISS** (29.233 s > 15 s, qual-run-7); claim 3 stays withdrawn; 3.009 s event loop still **not** a strict 3 s pass | `claim-rescope-signoff-2026-10-11.md`, `task-12-fix/qual-run-7/qual-receipt-run-7.md`, `task-12-bluez-lightweight.json` |

All six `probe-pass-provisional` entries are interim: before final release
approval they must resolve to `full-pass` or `owner-signed-reduced-scope`;
`--stage final-release` on this report currently exits 1 by design.

## Owner sign-offs (verbatim)

### decode (`owner-signed-reduced-scope`)

- owner: `Newton`
- timestamp: `2026-10-06T15:57:52Z`
- evidence_ref: `.omo/evidence/jetson-android-auto-receiver/task-9-decode-probe.json`
- statement: "Task 9 decode probe is accepted at reduced scope. The 600s Jetson
  qualification at approved SHA a0a2049 proved 1280x720@30 H.264 NVIDIA-hardware
  decode with 18000/18000 frames, zero drops, 30.00 fps, 10.02 Mbps (within 5%),
  and 3.83% CPU. I withdraw the p95 decode latency <=33 ms budget claim (measured
  p95 36.09 ms, avg 34.66 ms, consistent with one-frame decoder buffering per
  task-9-latency-analysis.md) and the software-fallback qualification claim
  (openh264dec run exceeded its deadline; path remains unqualified). Downstream
  tasks must not claim the 33 ms decode latency budget or a qualified software
  fallback; final release gate must label this reduced-scope."
- withdrawn_claims:
  1. `decode p95 latency at most 33 ms at 1280x720@30`
  2. `software H.264 fallback path qualification`

### wireless (`owner-signed-reduced-scope`)

Current sign-off (claim resolution, 2026-10-11):
- owner: `Newton`
- timestamp: `2026-10-11T06:41:02Z`
- evidence_ref: `.omo/evidence/jetson-android-auto-receiver/claim-rescope-signoff-2026-10-11.md`
- statement: "sign."
- resolved_claims:
  1. `Completed onboard RTL8822CE UNII-1 5 GHz AP qualification (ch 44, 20 MHz) at the early capability gate` — re-scoped from the channel-36 wording (the vendor driver forces a CSA 36→44); **demonstrated** (qual-run-1, qual-run-5, qual-ht20-test receipts)
  2. `Completed isolated AP client-association qualification at the early capability gate` — **demonstrated** (qual-client-assoc receipt)
- withdrawn_claims (remaining):
  1. `Full wireless capability acceptance as a prerequisite for continuing independent wired implementation`

### Post-sign-off wireless observations (2026-10-11/13, qual-run-6/7 — not part of the signature)

The sign-off above is unchanged: claim 3 stays withdrawn/open. Later qualification
windows produced bounded observations, none of which resolves claim 3:

- `wireless.ap_startup_s` (≤ 5 s) **measured and passes at its stated trigger**:
  modprobe-start → `AP-ENABLED` = 4.223 s worst of 3 journal-valid cycles, on the
  receiver-hosted `aa-qual-44` AP (ch 44, 20 MHz). This is not a boot-to-AP number.
- `wireless.session_reconnect_s` (≤ 15 s) **measured and MISSES**: 29.233 s worst of
  3 journal-valid cycles on the contract topology (receiver-hosted `aa-qual-44` AP,
  phone as client; session chain = re-association + adb-over-WiFi, task-12 pattern,
  owner-confirmed 2026-10-11). Steady-state cycles recover in ~10.3 s; cycle 1's slow
  first re-association (27.8 s; consistent with a full-channel rescan, scan evidence
  not separately captured) is the max-kind reading (qual-run-7 receipt).
  Interrupting via `iw station deauth` proved a driver no-op and was discarded (v1
  note in the receipt); the run used an AP-side RF-kill toggle with journal-enforced
  drop+recovery validity gates.
- `wireless.session_reconnect_s` was **not** measured by qual-run-6: that window
  exercised **infrastructure-STA** RF-kill recovery (Jetson as client of the venue
  infrastructure router) at 4.139 s — a different topology (recorded as the separate
  `infra_sta_rfkill_reconnect_s` observation). The 30 s ping sample (30/30, 0 % loss)
  is likewise infrastructure-STA only.

qual-run-7 (2026-10-13) exercised exactly that measurement on the receiver-hosted
session chain and recorded a **miss** (29.233 s > 15 s). What remains is either
engineering the recovery under 15 s and re-qualifying, or an owner-signed budget
withdrawal naming the miss (the decode p95 pattern).

Historical sign-off (2026-10-08, original withdrawal — preserved verbatim):
- owner: `Newton`
- timestamp: `2026-10-08T17:28:33Z`
- evidence_ref: `.omo/evidence/jetson-android-auto-receiver/task-12-lightweight-preflight.json`
- statement: "do not let that block us. find a command maybe. if not, we skip."
- withdrawn_claims (at that time):
  1. `Completed onboard RTL8822CE channel-36 AP qualification at the early capability gate`
  2. `Completed isolated AP client-association qualification at the early capability gate`
  3. `Full wireless capability acceptance as a prerequisite for continuing independent wired implementation`

These sign-offs never count as full compliance. The wireless release budgets
below were left unmeasured and unclaimed rather than withdrawn at signing; both
are now measured (qual-run-6/7): `ap_startup_s` passes, `session_reconnect_s`
**misses** its 15 s limit — see the dated observation section above.

## Budgets

Limits are fixed in `tools/gates/capability_budgets.py` (plan "Contract
budgets"); the checker rejects any report that redefines a limit, kind, unit,
area or availability stage.

### Probe-stage budgets (available now)

| budget | kind | limit | measured | verdict |
|--------|------|-------|----------|---------|
| decode.duration_s | min | 600 s | 600.0 s | pass |
| decode.fps_error_percent | max | 5 % | 0.0 % | pass |
| decode.bitrate_error_percent | max | 5 % | 0.2049768 % | pass |
| decode.dropped_frames | max | 0 | 0 | pass |
| decode.p95_latency_ms | max | 33 ms | 36.09 ms | **fail — withdrawn by owner** |
| decode.cpu_percent_one_core | max | 50 % | 3.83 % | pass |
| ipc.duration_s | min | 600 s | 600.000652 s | pass |
| ipc.throughput_mbps | min | 10 Mbps | 12.189683 Mbps | pass |
| ipc.p95_added_latency_ms | max | 33 ms | 0.2 ms | pass |
| ipc.happy_drops | max | 0 | 0 | pass |

### Release-stage budgets (provisional — two now measured, one passing; rest unmeasured, unclaimed)

| budget | kind | limit | area |
|--------|------|-------|------|
| audio.path_latency_ms | max | 200 ms | decode |
| media.avsync_ms | max | 50 ms | decode |
| session.projection_recovery_s | max | 60 s | ipc |
| soak.duration_h | min | 4 h | ipc |
| soak.crashes | max | 0 | ipc |
| soak.rss_growth_percent | max | 10 % | ipc |
| rss.receiver_mb | max | 250 MB | ipc |
| rss.helper_mb | max | 32 MB | ipc |
| receiver.cpu_sustained_percent_one_core | max | 50 % | ipc |
| usb.session_reconnect_s | max | 5 s | usb |
| wireless.session_reconnect_s | max | 15 s | wireless |
| wireless.ap_startup_s | max | 5 s | wireless |

Measured 2026-10-11 (qual-run-6): `wireless.ap_startup_s` = **4.223 s** (pass, ≤ 5 s;
3/3 journal-valid cycles; trigger = modprobe-start → `AP-ENABLED`, receiver-hosted
`aa-qual-44` AP — not boot-to-AP).

Measured 2026-10-13 (qual-run-7): `wireless.session_reconnect_s` = **29.233 s**
(**miss**, > 15 s; 3/3 journal-valid cycles on the receiver-hosted AP with the phone
as client; session chain = re-association + adb-over-WiFi; steady-state ~10.3 s,
cycle-1 slow first re-association — consistent with a full-channel rescan, scan
evidence not separately captured — is the max-kind reading). The 4.139 s figure from
qual-run-6 is a different topology (infrastructure-STA) and is recorded separately as
`infra_sta_rfkill_reconnect_s`. All other release budgets remain unmeasured and
unclaimed.

## Explicit non-claims

- Audio, USB-session-reconnect, recovery and soak budgets are **unmeasured**; no
  early probe result claims them as passed. The wireless session-reconnect budget is
  **measured and misses** its limit (see below) — it is not claimed as passed either.
- USB: AOA2 capability only. No Android Auto session and no bulk transfer were
  exercised; this is **not** projection and not a session-reconnect pass.
- Wireless: UNII-1 5 GHz AP (ch 44, 20 MHz) and isolated client-association are
  **demonstrated** (2026-10-10/11 receipts); the channel-36 wording was re-scoped
  per the 2026-10-11 sign-off (the driver forces a CSA 36→44 — evidence kept, not
  silently passed). The BlueZ event loop observed 3.009 s against a 3 s deadline —
  **not** a strict wall-bound pass. AP mode advertisement is not AP operation. No
  pairing or trust claims. `ap_startup_s` is measured and passes (4.223 s, trigger
  modprobe-start → `AP-ENABLED`); `session_reconnect_s` is **measured and misses**
  (29.233 s > 15 s, qual-run-7, receiver-hosted AP with the phone as client;
  steady-state ~10.3 s, cycle-1 slow first re-association — consistent with a
  full-channel rescan, scan evidence not separately captured — is the max-kind
  reading) — it is
  not claimed as passed. qual-run-6's 4.139 s figure is infrastructure-STA RF-kill
  recovery (the Jetson's upstream link to the venue router), **not** the
  receiver-hosted AA session
  transport and not an AA session; a 30 s ping sample (30/30, 0 % loss) is likewise
  infrastructure-STA only. Full wireless capability acceptance stays deferred
  (claim 3 withdrawn/open); channel-36 AP stays out of the signed scope (B-route
  patch is future work).
- Decode: the 33 ms p95 latency budget is **not** met (36.09 ms measured) and
  the software H.264 fallback path is **unqualified**; both claims are
  withdrawn and must not be used downstream or in release compliance claims.
- IPC: host-only synthetic bytes; not ARM64 runtime, not valid-H.264 decode,
  not real-phone projection, not end-to-end media latency.
- Dependencies/ABI/TLS are host and cross-build evidence only; no Jetson
  runtime execution, real-phone or production-TLS qualification is claimed.

## Evidence digests

Each evidence name above is a receipt under
`.omo/evidence/jetson-android-auto-receiver/`; the machine report records the
SHA-256 of every receipt it cites. The checker validates those digests are
well-formed but never reads the ignored `.omo/` tree.
