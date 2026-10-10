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
| wireless | **owner-signed-reduced-scope** | AP/client deferred; BlueZ registration + clean unregister succeeded; 3.009 s event loop is **not** a strict 3 s deadline pass | `task-12-bluez-lightweight.json`, `task-12-owner-signoff.json` |

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

- owner: `Newton`
- timestamp: `2026-10-08T17:28:33Z`
- evidence_ref: `.omo/evidence/jetson-android-auto-receiver/task-12-lightweight-preflight.json`
- statement: "do not let that block us. find a command maybe. if not, we skip."
- withdrawn_claims:
  1. `Completed onboard RTL8822CE channel-36 AP qualification at the early capability gate`
  2. `Completed isolated AP client-association qualification at the early capability gate`
  3. `Full wireless capability acceptance as a prerequisite for continuing independent wired implementation`

These sign-offs never count as full compliance. The wireless withdrawn claims
are capability-level: the wireless release budgets below stay unmeasured and
unclaimed rather than withdrawn.

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

### Release-stage budgets (provisional — not measured, not claimed)

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

## Explicit non-claims

- Audio, reconnect, recovery and soak budgets are **unmeasured**; no early
  probe result claims them as passed.
- USB: AOA2 capability only. No Android Auto session and no bulk transfer were
  exercised; this is **not** projection and not a session-reconnect pass.
- Wireless: AP/channel-36/client qualification deferred (owner-signed). The
  BlueZ event loop observed 3.009 s against a 3 s deadline — **not** a strict
  wall-bound pass. AP mode advertisement is not AP operation. No pairing or
  trust claims. Wireless-dependent work stays deferred, not silently passed.
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
