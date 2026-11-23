# Video profiles and pipeline options

The receiver offers an ordered list; the phone selects an announced
configuration index. The **provisional default** remains
**1280x720@30 H.264, then 800x480@30 H.264**. It is defined in the single return
statement of `aa::config::default_video_profiles()`; changing the default after
benchmarking is a one-line configuration change. No resolution or rate is
rejected for performance reasons.

```
aa-wired-receiver --port PORT --decoder software --video-profiles 2560x1440@60:h264,1920x1080@60:h265,1280x720@30:h264
aa-wireless-receiver --store-dir DIR --phone-id N --video-profiles 1920x1080@60:h264,1280x720@30:h264
aa-dummy-display --decoder software --presentation cpu --media-thread gui --video-profiles 2560x1440@60:h264,1920x1080@60:h265,1280x720@30:h264
```

The reference display can start before the receiver and stays open across
receiver restarts. It shows `unavailable` while disconnected. After it
re-registers, press F5 to request projection, or use `--test-only-autostart`
to request projection after every attachment.

`--video-profiles` accepts comma-separated `WIDTHxHEIGHT@FPS:CODEC`, without
spaces. Both receivers parse it with the same `aa::config` helper, so a wireless
service offer names the profiles it was configured with. Every video combination
in the vendored protocol is valid:

| Axis | Protocol values |
| --- | --- |
| Landscape | 800x480, 1280x720, 1920x1080, 2560x1440, 3840x2160 |
| Portrait | 720x1280, 1080x1920, 1440x2560, 2160x3840 |
| Frame rate | 30, 60 |
| Codec | h264, h265, vp9, av1 |

All 72 combinations can be offered, in caller preference order. Empty lists,
duplicates, unknown protocol values and trailing commas are refused. The
receiver validates protocol shape and decoder availability, **not throughput**.
`--screen-*` pixel quantities are in the first offered profile's pixels and
scale per axis for every fallback, as in #103. Panel density and non-pixel
properties remain unchanged.

READY announces every index compatible with the channel's SETUP codec. START
must select an index actually announced by that channel; audio still announces
only index zero. An unannounced index is a protocol error. A duplicate START
cannot silently change profile within the same session. Configured 1080p,
1440p, 4K, portrait and 60 fps profiles use the same acceptance path as 720p30.

The chosen profile travels with media delivery and across the video socket.
Before submitting the first access unit, the display configures its decoder
with the selected resolution, fps and codec. Decoded dimensions drive image
size/aspect ratio; fps drives decoder duration and presentation telemetry.
Touch retains the receiver's registered viewport coordinate space. A
`video_profile` qualification record carries width, height, fps and codec once
when media begins for a session. This is negotiation, not a physical panel
refresh or scanout measurement.

## Runtime codec discovery

Use the same `--decoder software|hardware|auto` and offered profile list on the
receiver and display. The receiver checks the decoder admission policy locally
before announcing anything. The display checks its configured list at startup
and probes the actual selected codec again when opening a decode session.
`auto` uses an installed usable hardware path, otherwise software. Explicit
hardware never silently downgrades to software.

| Codec | Parser | Software candidates, preferred first | Hardware |
| --- | --- | --- | --- |
| H.264 | h264parse | openh264dec, avdec_h264 | nvv4l2decoder |
| H.265 | h265parse | avdec_h265 | nvv4l2decoder |
| VP9 | vp9parse | vp9dec, avdec_vp9 | nvv4l2decoder |
| AV1 | av1parse | dav1ddec, av1dec | nvv4l2decoder |

The existing H.264 automatic probe also knows desktop `nvh264dec`; explicit
Jetson hardware admission requires `nvv4l2decoder`. Factory presence is checked
at runtime, decoder sink caps must accept the requested codec, and the existing
probe checks READY viability. The NVMM output path additionally requires
`nvvidconv`. All paths require appsrc, the codec parser, videoconvert and
appsink. Missing paths return `channel_decode_unavailable` with the codec,
missing elements and detection causes. There is no hard-coded ban on H.265,
VP9 or AV1. Installed software paths use the matching parser and encoded caps.

Inspect the host without starting a session or opening a GUI:

```
aa-wired-receiver --list-video-capabilities
aa-dummy-display --list-video-capabilities
gst-inspect-1.0 nvv4l2decoder
gst-inspect-1.0 avdec_h265
```

The listing emits one JSON record per codec, with parser-path, software and
hardware availability plus installed/usable decoder elements. Availability
says nothing about sustainable resolution or frame rate. VP9 uncompressed
headers and AV1 low-overhead OBU headers identify recovery frames; H.264 uses
IDR NALs and H.265 uses IRAP NALs. Header detection follows the
[WebM VP9 specification](https://downloads.webmproject.org/docs/vp9/vp9-bitstream_superframe-and-uncompressed-header_v1.0.pdf)
and [AV1 specification](https://aomediacodec.github.io/av1-spec/av1-spec.pdf).

## Pipeline options and capability matrix

The provisional display default is **software + cpu + gui**. The three axes
live in one validated `PipelineOptions` structure:

| Axis | Values | Status |
| --- | --- | --- |
| `--decoder` | software, hardware, auto | Runtime element/caps admission; existing decoder adapter |
| `--presentation` | cpu, gpu, zerocopy | CPU implemented; GPU/zero-copy follow in later PRs |
| `--media-thread` | gui, worker | GUI implemented; worker follows in a later PR |

On this laptop without NVIDIA, hardware, GPU and zero-copy return a clear
“not available on this host” error naming `nvv4l2decoder`. On a host with the
NVIDIA decoder, GPU and zero-copy remain “not yet implemented” in this PR;
zero-copy additionally requires an explicit hardware decoder. Worker pulling
is “not yet implemented”. Unsupported selections never silently downgrade.
The pre-existing NVIDIA-to-system-memory adapter is reused, with no new
hardware implementation or Jetson qualification claimed here.

`--decoder-backend` retains its original lazy semantics: software/nvidia/
automatic select the existing DecodeHelper policies only when projection
starts, so idle startup can still succeed without the requested decoder.
It shares the options structure and presentation/thread validation. The new
`--decoder` performs early admission. Legacy NVIDIA never silently falls back;
legacy automatic may. The default decoder is now explicitly software.

`--check-pipeline` checks availability and exits before bus/socket attachment.
Run the small matrix on a private session bus:

```
dbus-run-session -- python3 -B tools/dummy-display/pipeline_matrix.py --display build/host-dev/aa-dummy-display
```

It probes all 18 combinations, starts and cleanly quits each available display,
and writes JSON records plus exact ran/unavailable/failed totals. The laptop
runs software/cpu/gui and auto/cpu/gui; the remaining 16 are unavailable.
This is a startup smoke matrix, not a decode or performance benchmark. The
consumer suite separately decodes synthetic H.265, VP9 and AV1 single-frame
fixtures whenever their software elements are available.

## IPC compatibility

Legacy senders retain the byte-identical version 1, 56-byte data header with a
zero reserved word. Profile-bearing access units use data version 2 with the
same header size: offset 52 holds a big-endian word `resolution << 16 | fps << 8
| codec`. Resolution IDs are the vendored enum order 1–9 (the table above);
codec IDs are 3 (H.264), 5 (VP9), 6 (AV1), 7 (H.265). All fragments must carry
identical metadata. Invalid values and inconsistent fragments are rejected.
The limits handshake stays version 1. Upgrade receiver and display together:
older displays reject version 2. Legacy producers without metadata retain
DisplayConfig caps. Protocol validity and observed output-dimension validation
remain enforced; the old 720p30-only decoder guard is removed.

## Video IDR recovery

When video frames are dropped (`backpressure`, `waiting_idr`, `no_consumer` before
first attachment, consumer disconnect/reattach, or decoder `awaiting_idr`), the
pipeline suppresses non-IDR access units until a fresh IDR arrives and requests a
keyframe from the phone while projecting:

- `--idr-recovery auto|nudge|transition` (default `auto`) configures the wired
  and wireless receivers' recovery step policy:
  - `auto`: sends an unchanged projected `VideoFocusNotification` (`unsolicited=true`)
    as the first cheap nudge (rate-limited at `kIdrRequestMinInterval = 250 ms`),
    and escalates to an ordered focus transition (`native` followed by the prior
    projected mode, `projected` or `projected_no_input_focus`) if no IDR arrives
    within `kIdrEscalationDelay` (`1000 ms`) or `kIdrEscalationMaxNudges` (`3` nudges),
    following the focus-toggle workaround documented in
    `aa-proxy/aa-proxy-rs@2af9270` (#142).
  - `nudge`: only sends projected `VideoFocusNotification` nudges.
  - `transition`: performs the ordered `native` -> prior focus transition directly,
    bounded by `kIdrTransitionCooldown` (`5000 ms`).
- Escalation emits `idr_escalated` naming `step` (`focus_transition`). Focus
  transitions are rate-limited by `kIdrTransitionCooldown` (`5000 ms`), and both
  per-request and transition cooldown timestamps persist across episode
  boundaries so rapid IDR/drop cycles or interleaved D-Bus `RequestIdrRecovery`
  calls cannot burst focus notifications.
- Late consumer attachment arms recovery if frames arrived before the first
  consumer connected (or after `kIdrStartupGrace = 1000 ms` waiting for an initial
  IDR on either endpoint).
- `idr_recovered` reports both `episode_requests` and lifetime `total_requests`.
  Endpoint `latency_ms` differs intentionally by endpoint:
  - `source: "producer"`: time from entering wait (`begin_wait()`) until the
    producer dispatches the recovery IDR onto the IPC socket (`SendStatus::sent`).
  - `source: "consumer"`: time from entering wait (`begin_wait()`) until the
    display decoder exits `awaiting_idr` and socket resync clears.

## Video IPC socket buffer sizing

`VideoProducer::configure_send_buffer()` and
`VideoConsumer::configure_receive_buffer()` request a profile-scaled buffer target
from `recommended_socket_buffer_bytes()` (`256 KiB`–`4 MiB`) using `setsockopt`
without mutating sysctls or using `SO_SNDBUFFORCE`. On Linux/Jetson kernels with
stock sysctls (`net.core.wmem_max = net.core.rmem_max = 212992`), the kernel
clamps the requested option above at `wmem_max`/`rmem_max` and floors the
doubled accounting value below at `SOCK_MIN_SNDBUF`/`SOCK_MIN_RCVBUF`:
`getsockopt()` returns the authoritative `granted_raw_bytes` kernel option value
(`max(2 * min(requested, sysctl_max), SOCK_MIN_*)`, e.g., `425984` when capped at
`212992`), while `granted_bytes` reports the normalized Linux option value
(`granted_raw_bytes / 2`, e.g., `212992`). Because `getsockopt()` returns the
unchanged socket option even when `setsockopt()` fails, `granted_bytes` only
reflects the applied grant when `setsockopt_errno == 0 && getsockopt_errno == 0`,
and `granted_raw_bytes` plus the errno fields remain authoritative. In addition,
`SO_RCVBUF` has no queueing effect on `AF_UNIX` `SOCK_SEQPACKET` beyond the sender
buffer. Whenever `short_grant()` (`setsockopt_errno == 0 && getsockopt_errno == 0
&& granted_bytes < requested_bytes`) or a socket option error occurs,
`video_socket_buffer` records the raw and normalized option counts alongside a
non-fatal `video_socket_buffer_warning` event. Extra IPC burst headroom above
`212992` bytes is not available on stock sysctls and requires an explicit
administrator setting such as `net.core.wmem_max=4194304` (4 MiB).

## Transient backpressure hold

When a large IDR keyframe (e.g., ~214 KB at 1440p60 H.265) fills a short-granted
`212992`-byte kernel `SO_SNDBUF`, the next P-frame ~4 ms later can hit
`EAGAIN`/`EWOULDBLOCK` before the display consumer completes its 5 ms pump /
~28 ms Jetson paint cycle. Immediately dropping that P-frame into `waiting_idr`
forces a periodic IDR recovery loop even though the socket drains momentarily.

Both wired and wireless receivers share `aa::wired_receiver::VideoOutlet`, which
holds whole access units in a strictly bounded FIFO pending queue via non-dropping
`VideoProducer::try_send_access_unit()` (resuming mid-AU fragment index if `EAGAIN`
occurs mid-AU without breaking `VideoReassembler`'s monotonic sequence contract):

- **Bounds (`aa::ipc::recommended_backpressure_budget`)**:
  - **Age (`max_age`)**: `clamp(ceil(2000 / fps), 50 ms, 100 ms)` — `50 ms` at 60 fps,
    `67 ms` at 30 fps, capped at `100 ms`. This rides out one display pump + paint
    jitter window (~33 ms on Jetson at ~36 fps) while keeping worst-case queueing
    bounded before escalating to keyframe recovery.
  - **Bytes (`max_bytes`)**: `clamp(recommended_socket_buffer_bytes(profile) / 4, 256 KiB, 1 MiB)`
    (`256 KiB` up to 1080p30, `486,000 B` at 1080p60, `864,000 B` at 1440p60, `1 MiB`
    at 4K60, `256 KiB` when profile metadata is absent) — roughly one recovery
    keyframe / ~3–4 P-frames so memory stays strictly bounded and cannot absorb a
    sustained downstream stall.
- **Draining**: Held frames flush in strict FIFO order on the next incoming AU
  (`VideoOutlet::on_message`) and periodically via `VideoOutlet::service_pending()`
  under the existing outbound-message and `idr_dispatching` guards. On wired USB,
  `receive_with_audio()` bounds `poll_transport()` to `5 ms` while frames are
  held. On wireless TCP, `TcpTransport::poll_receive()` is intentionally
  unsupported so `WirelessLink::poll()` still runs blocking `poll_transport()`,
  which services `Runtime::checkpoint()` in `25 ms` `TcpIo` slices when TCP is
  idle (`Runtime.hpp` also shortens its outer sleep to `5 ms` on non-blocked
  turns); accordingly, when TCP is quiet during a hold, drain/expiry on wireless
  can overshoot the nominal `max_age` by up to one `~25 ms` slice (still
  strictly finite).
- **Overflow & reset**: Exceeding either `max_age` or `max_bytes` drops all held AUs
  (and the overflowing AU if any), logs `video_dropped` with `reason: "backpressure"`,
  enters `waiting_idr` (`enter_waiting_idr_on_backpressure()`), and triggers IDR
  recovery identically to the prior drop-and-report policy. Consumer disconnect,
  consumer replacement (`install`), stream profile/session/generation change, session
  `reset()`, and `close()` clear the pending queue immediately (without emitting
  `video_backpressure_released`), account for discarded held AUs, and force
  `waiting_idr` on any retained producer regardless of whether any fragment of the
  held AU escaped.
- **Telemetry**: Emits `video_backpressure_held` (`held_units`, `held_bytes`,
  `age_ms`, `max_bytes`, `max_age_ms`) on entering a hold episode and
  `video_backpressure_released` (`held_units`, `held_bytes`, `age_ms`) once the
  held queue drains onto the socket. Real overflows still emit `video_dropped`
  (`reason: "backpressure"`).

## Following PRs and qualification

1. Implement/optimize and qualify the Jetson hardware decode path.
2. Implement media pulling on a worker with explicit lifetime and GUI delivery.
3. Implement GPU YUV textures and shader presentation.
4. Implement NVMM/DMABUF to EGLImage to GL_TEXTURE_EXTERNAL_OES zero-copy.
5. Benchmark every available combination at every configured profile through
   the existing 1800 s and 7200 s stages (the 60 s stage is retired; a
   same-phone drive smoke gates them): A/V skew <=50 ms, audio latency
   <=200 ms, receiver CPU <=50% of one core, RSS <=250 MB and growth <10%, zero
   crashes, and the existing Orin Nano thermal policy. Record copy/upload costs,
   frame loss and per-thread load. Decode p95 remains informational. Choose the
   documented default from those results, rather than rejecting profiles in
   validation because they may be expensive.

Qt `frameSwapped` means queued for presentation, not measured scanout. Phone
codec/profile support, hardware decode and sustained high-resolution throughput
still require on-target qualification.
