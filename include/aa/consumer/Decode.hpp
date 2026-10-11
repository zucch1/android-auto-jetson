// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/core/Time.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/protocol/Protocol.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aa::consumer {

// Consumer-side GStreamer H.264 decode helper (libaareceiver-consumer). It
// consumes reassembled encoded access units as delivered by the task-22 video
// channel (aa::ipc::VideoAccessUnit) and hands decoded frames back through a
// caller-supplied callback, carrying the channel's monotonic A/V correlation
// metadata (frame id, wire sequence, timestamp_ns) through to every output.
//
// Task-9 reduced scope (owner-signed 2026-10-06,
// .omo/evidence/jetson-android-auto-receiver/task-9-owner-signoff.json): the
// decode p95 latency budget (<=33 ms at 1280x720@30) and the SOFTWARE H.264
// FALLBACK qualification are WITHDRAWN claims. This helper, its tests and its
// evidence record measured budgets but never claim them as met.
//
// Ownership/threading: the helper is caller-driven and owns no threads of its
// own (GStreamer runs its normal streaming threads inside the pipeline). One
// thread at a time calls submit/end_of_stream/pump/close; the frame callback
// runs on the pump() caller's thread, never on a GStreamer thread. close() is
// idempotent and the destructor runs the same release.
//
// Backend honesty (mirrors tools/aa-decode-probe): the NVIDIA path is
// runtime-detected and preferred when its COMPLETE output path is usable.
// There is no compile-time assumption that JetPack implies the plugins. An
// NVMM-only decoder (nvv4l2decoder) is usable only together with the
// runtime-detected NVMM conversion (nvvidconv); if that conversion or the
// pipeline construction is unavailable, the NVIDIA candidate is explicitly
// disabled with a recorded cause in the DecodeReport and automatic mode falls
// back to the software backend. Explicit DecodeBackend::nvidia requests never
// silently downgrade: they fail typed instead. This helper's pipeline is NOT
// the todo-9 probe pipeline and inherits none of its qualification: the
// NVIDIA path's qualification claim stays limited to what the todo-9 probe
// certified.
//
// Caps negotiation: exactly the channel budget profile
// (protocol::valid_video_profile): 1280x720@30 primary, 800x480@30 fallback.
// Anything else fails negotiation with channel_caps_unsupported before a
// pipeline is built; observed output dimensions that differ from the
// negotiated caps also fail typed, never silently falling back to wrong
// dimensions.

// Decoder backend selection policy for open().
enum class DecodeBackend {
    automatic, // NVIDIA when viable, otherwise software
    software,  // host software H.264 decoder only
    nvidia,    // NVIDIA (nvv4l2decoder) only; typed error when not viable
};

// One known decoder element and its runtime detection result (the probe's
// honesty pattern: presence and viability are measured, never assumed).
// `viable` means a COMPLETE usable output path, not just a READY transition:
// NVMM-only decoders (nvv4l2decoder emits only video/x-raw(memory:NVMM), per
// the task-9 element inspection) additionally require the runtime-detected
// NVMM conversion element before they count as viable. apply_output_path_policy
// folds that rule in and records the disable cause here when the path is
// incomplete.
struct BackendCandidate final {
    std::string element;   // GStreamer element name (e.g. "nvv4l2decoder")
    bool hardware{};       // NVIDIA-class element per the probe's known table
    bool requires_nvmm_conversion{}; // NVMM-only decoder output
    bool available{};      // factory present in the GStreamer registry
    bool viable{};         // complete usable path (decoder READY + conversion)
    std::string cause;     // recorded note/cause for availability and viability
};

// Runtime decode report: full detection table plus selection and recorded
// enable/disable causes. Counters grow across submit/pump calls. On
// probe_backends() (no session open) nvidia_enabled/nvidia_cause report NVIDIA
// viability; after open() they describe the selection actually made.
struct DecodeReport final {
    std::vector<BackendCandidate> backends{};
    bool nvmm_conversion_available{};  // nvvidconv runtime-detected and READY-viable
    std::string nvmm_conversion_cause{}; // recorded detection note for the conversion
    DecodeBackend requested{DecodeBackend::automatic};
    std::string selected_element{};
    bool nvidia_enabled{};
    std::string nvidia_cause{};      // recorded cause for enable or disable
    std::string negotiated_caps{};   // observed output caps (set on first frame)
    std::uint64_t frames_delivered{};
    std::uint64_t frames_unmatched{}; // outputs whose PTS matched no submitted AU
};

// Pure output-path policy (no GStreamer calls): a READY-viable NVMM-only
// decoder is usable only when the NVMM conversion is available. Otherwise the
// candidate is disabled and its cause records the missing conversion. Exposed
// so tests can pin the rule even where the environment cannot (Jetson elements
// on a non-Jetson host).
void apply_output_path_policy(BackendCandidate& candidate, bool nvmm_conversion_available);

// Pure selection result over a probed table (no GStreamer calls).
struct BackendSelection final {
    std::string element{};   // empty when usable == false
    bool hardware{};
    bool nvidia_enabled{};
    std::string nvidia_cause{};
    bool usable{true};       // false: the request cannot be satisfied (typed error)
};

// Pure selection policy (no GStreamer calls): first viable hardware candidate
// for automatic/nvidia, first viable software candidate for automatic/software.
// usable == false when the request has no viable candidate; open() maps that to
// channel_decode_unavailable (explicit NVIDIA requests never silently downgrade).
[[nodiscard]] BackendSelection select_backend(const DecodeReport& report, DecodeBackend request);

// Decoded I420 frame with the channel's monotonic correlation metadata carried
// through unchanged from the submitted access unit. i420 is tightly packed:
// Y (width*height) + U (width*height/4) + V (width*height/4).
struct DecodedFrame final {
    std::uint64_t frame{};         // channel access-unit id
    std::uint64_t sequence{};      // wire sequence of the AU's first fragment
    std::uint64_t timestamp_ns{};  // monotonic A/V correlation timestamp
    std::uint16_t width{};
    std::uint16_t height{};
    std::vector<std::uint8_t> i420{};
};

using FrameCallback = std::function<void(const DecodedFrame&)>;

struct DecodeConfig final {
    protocol::VideoProfile caps{};   // must satisfy protocol::valid_video_profile
    DecodeBackend backend{DecodeBackend::automatic};
    FrameCallback on_frame{};
};

class DecodeHelper final {
public:
    // Runtime decoder detection over the probe's known candidate table
    // (nvv4l2decoder, openh264dec, avdec_h264, nvh264dec): no pipeline is
    // built, nothing is selected. Always safe to call before open().
    [[nodiscard]] static DecodeReport probe_backends();

    // Caps negotiation + backend selection + pipeline construction.
    // channel_caps_unsupported: caps outside the channel budget profile.
    // channel_decode_unavailable: no viable decoder backend for `config.backend`.
    // channel_decode_failed: the GStreamer pipeline could not be started.
    [[nodiscard]] static core::Result<DecodeHelper> open(DecodeConfig config);

    ~DecodeHelper();

    DecodeHelper(const DecodeHelper&) = delete;
    DecodeHelper& operator=(const DecodeHelper&) = delete;
    DecodeHelper(DecodeHelper&& other) noexcept;
    DecodeHelper& operator=(DecodeHelper&& other) noexcept;

    // Feed one encoded access unit (task-22 framing metadata). The unit's
    // timestamp_ns becomes the buffer PTS and must be unique per open session.
    // invalid_argument: empty payload, duplicate timestamp, or use after
    // close/end_of_stream. ipc_queue_full: bounded feed backlog exceeded.
    // channel_decode_failed: the pipeline rejected the data.
    [[nodiscard]] core::Result<void> submit(const ipc::VideoAccessUnit& access_unit);

    // Signal end of encoded input (drain is completed by pump()).
    [[nodiscard]] core::Result<void> end_of_stream();

    // Pull decoded frames and deliver them to the frame callback. `wait` bounds
    // the first pull; the rest drain non-blocking. Returns frames delivered.
    // channel_caps_unsupported: observed output dimensions differ from the
    // negotiated caps. channel_decode_failed: pipeline error on the bus.
    [[nodiscard]] core::Result<std::size_t> pump(core::Milliseconds wait);

    // Idempotent teardown; the destructor runs the same release.
    void close() noexcept;

    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] const DecodeReport& report() const noexcept;

private:
    struct Impl;
    explicit DecodeHelper(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace aa::consumer
