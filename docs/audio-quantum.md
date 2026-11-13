# Real-time audio sink policy (#80)

The wired receiver refuses the native GStreamer `pipewiresink` route outright.
`pulsesink` (via `pipewire-pulse`) is the default and only supported real-time
route (`--audio-sink automatic` selects `pulsesink`, never `pipewiresink`).

Why: on the target platform (PipeWire 1.0.5 on Ubuntu noble), `pipewiresink`
has no rate control and degrades after 1.5-2.3 hours of playback, and no fixed
PipeWire release is packaged for Ubuntu noble. (Graph quantum and timing ARE
readable on PipeWire 1.0.5 via `libpipewire` `pw_time.size` /
`SPA_IO_Position`; unobservability is not the reason `pipewiresink` is refused.)

Behavior at startup and runtime:

- `SinkBackend::automatic` and `SinkBackend::pulse` select `pulsesink`, and
  fail closed if `pulsesink` is absent or unavailable rather than falling back
  to `pipewiresink`.
- An explicit `--audio-sink pipewire` (or `SinkBackend::pipewire`) fails at
  startup with `ErrorCode::audio_sink_unavailable`, logs `audio_sink_selected`
  with the refusal cause naming `pulsesink` as the supported route, and exits
  without opening playback or falling back.

The test-only native adapter helpers remain private in internal implementation
files for existing native integration tests and are never called by production
code. `audio_pipeline_sink_policy` and `WiredReceiver` tests cover explicit
`pipewire` refusal, the `pulsesink` default, and no-fallback behavior; long-run
Jetson/phone runs (1800 s / 7200 s) remain separate target qualification.
