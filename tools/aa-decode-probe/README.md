# Local decode probe: limits and reproduction

Task9 remains local-only and unaccepted. This tool does not establish Jetson,
phone, display, or target/hardware qualification. `target_qualified` is always
false. No Task8 integration is implied.

From the repository root of this worktree:

```sh
cmake -S tools/aa-decode-probe -B build/local-probe-correction -G Ninja
cmake --build build/local-probe-correction --parallel 4
ctest --test-dir build/local-probe-correction --output-on-failure
```

The local clangd files use the compilation database from this build directory.
GStreamer development packages and host `openh264dec` are required for the
software smoke scenario. Unit tests cover strict numeric parsing, JSON escaping,
nearest-rank p95, exact PTS correlation, and explicit decoder classification.
The CLI regressions include a Linux `/proc`/SIGSTOP fault-injection test that
stops the isolated worker; the parent must kill and reap it at the deadline.

## Requested versus observed

Reports use `aa-decode-probe/2` because the pass semantics and measured/requested
distinction are intentionally corrected; historical `/1` receipts are retained.

`profile` and `bitrate` are request labels, not fixture measurements. Explicit
`requested_*` fields carry their parsed values. `frames_fed` counts successfully
enqueued input access units; `frames_decoded` counts actual appsink samples.
`observed_width`/`observed_height` come from decoded caps. `observed_fps` is
decoded samples divided by total wall time, including drain. It is not an
assertion of the source stream's frame rate: the feeder synthesizes PTS and
paces replay at the requested rate. `observed_bitrate_bps` is the successfully
fed encoded bytes times eight divided by measured feed wall time. Neither the
bitrate option nor the profile option transcodes or modifies the fixture.

`smoke_pass` requires nonzero decoded frames, complete one-to-one PTS matches,
no drops, matching resolution, p95 <= 33 ms, process CPU <= 50% of one core,
and no pipeline/EOS/shutdown failure. `pass` additionally requires observed
rate and bitrate within 5% of the requested values. Even a local `pass` is
not target qualification. Workload checking is meaningless for unsupported
fixtures; those fail rather than infer frame boundaries or decode order.

## Historical fixture retained unchanged

The existing public synthetic fixture is retained, not replaced:

- `tests/fixtures/media/h264_720p30_baseline_90f.h264`
- introduced by `5e105d847008749887328228deb2563b51036fc2`
- SHA-256 `0b34e280254d1b7d4832a2e8b2bb4325e3de24f228127cf6ac03e600749f2265`
- 22,158 bytes, 90 frames, 1280x720 Baseline H.264

Its nominal 30 fps filename describes intended replay pacing, not negotiated
frame rate (the existing stream decodes with `framerate=0/1`). One complete
90-frame replay at 30 fps has about 59,088 encoded bits/s; shorter replay
windows vary with access-unit sizes. This fixture cannot honestly qualify a
10 Mbps decode workload. It now produces `smoke_pass=true`, `pass=false` for
the software smoke scenario requesting 10M. There is no new persistent media
fixture and no invented generation command. The corrupt AUD payload used by
the regression runner is a temporary, explicitly malformed test input.

The feeder accepts only AUD-delimited Baseline streams (no B frames), assigns
equal monotonic PTS/DTS, and groups all slices within each AUD access unit.
Exact output PTS lookup survives output reorder or drops without assigning the
next frame's latency; unknown or duplicate output PTS fail correlation. Other
profiles or streams without AUDs need a different validated feeder and are
not supported by this bounded correction.

Input is limited to 64 MiB; runtime to 1..3600 seconds; dimensions to 1..16384;
and requested pacing to 1..240 fps. The input queue/backlog is bounded and
push is nonblocking. EOS drain has a five-second deadline. The isolated worker
has an overall duration-plus-eight-second deadline, covering plugin discovery,
state changes, decode, and shutdown. This is userspace fault containment, not
a guarantee against an unkillable kernel/driver task. JSON-output write failure
returns a nonzero exit status.

Known NVIDIA factories `nvv4l2decoder` and `nvh264dec` are consistently marked
hardware; arbitrary names containing `nv` are not. READY-state viability and
hardware classification are discovery metadata, not evidence of target
compatibility or performance. Auto discovery may select the desktop NVCODEC
factory on this host; the existing software smoke test forces `openh264dec`.
