# Real-time audio sink capacity (#80)

The wired receiver defaults to `pulsesink` since #88 (`92a2bb0`). The
production router now also chooses that ring-buffer sink for `automatic`.
There is no ALSA or other production sink path. [Pulse's ring-buffer commit](https://github.com/GStreamer/gstreamer/blob/1.24.2/subprojects/gst-plugins-good/ext/pulse/pulsesink.c#L1452)
splits/copies audio into segments; it does not consume one receiver buffer
per PipeWire graph cycle. Its buffer/segment sizes can affect latency and
stalls, but need not be smaller than `render_quantum` to maintain throughput.
The 21,333 us latency-time request and default buffer-time are unchanged.
Playback time, handoff barriers, write deadlines, and the 50 ms render bound
are unchanged. #78's recovery from sink stalls remains separate from capacity.

Native `pipewiresink` retains the dependency described in #80. A completed
handoff probe proves connectivity, not sustainable capacity. The [PipeWire 1.0.5 plugin](https://github.com/PipeWire/pipewire/blob/1.0.5/src/gst/gstpipewiresink.c)
public properties expose neither the negotiated graph quantum nor
changes to it. A GStreamer latency query is not an observation of that graph
quantum. [`node.latency` is a suggestion](https://docs.pipewire.org/page_man_pipewire-props_7.html),
and even `node.lock-quantum` / `node.force-quantum` can be overridden by
metadata or another forcing node. Logging a requested quantum as observed
would give false assurance.

The conservative policy is to refuse this unobservable capacity at sink open:
`audio_sink_quantum_unverified` is recorded in the sink report and the
receiver's `audio_sink_selected` log. An explicit `--audio-sink pipewire`
opens degraded (`audio_sink_unavailable` on submission); it never silently
switches outputs. `automatic` records that reason while choosing Pulse,
and fails closed when Pulse is absent. No production native stream can become
active and then silently change to an unsafe quantum. Retrying/reopening uses
the same policy. The guard rejects unknown capacity for small and large render
bounds alike; it does not claim to measure a particular quantum.

The native adapter is retained solely for qualification of its existing
handoff/readiness/logging/output-loss contracts through private, explicitly
unverified test openers. No receiver calls those openers. Removing this refusal
would require actual per-stream negotiated-quantum observation (including
changes), a verified pin, or a different buffering implementation.

`audio_pipeline_quantum_guard` runs without an audio server, including a
viable native-only fake registry, both viable factories, sink-open refusal,
and typed router degradation. The receiver tests cover the existing Pulse
default and the actual log formatter. Existing native plugin integration gates
are preserved, not replaced by these host tests.

Remaining qualification: real Jetson/phone 1800 s and 7200 s runs using
`automatic`/Pulse, including forced 4096/8192-frame graph quanta and changes
midstream, with the existing latency/A/V/stall gates. Record server quantum
and buffer settings externally for those runs; this change adds no negotiated
quantum telemetry. No physical playback or long-run result is claimed here.
