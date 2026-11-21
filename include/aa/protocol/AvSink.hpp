// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace aa::protocol {

enum class AvSinkCodec : std::uint8_t { pcm = 1, h264 = 3, vp9 = 5, av1 = 6, h265 = 7 };
struct AvSinkSetup final { AvSinkCodec codec{AvSinkCodec::pcm}; };
struct AvSinkStart final {
    std::int32_t session_id{};
    // Wire value only; the stateful wrapper must check advertised membership.
    std::uint32_t configuration_index{};
};
struct AvSinkStop final {};

enum class VideoFocusMode : std::uint8_t {
    projected = 1, native = 2, native_transient = 3, projected_no_input_focus = 4
};
struct VideoFocusRequest final { std::optional<VideoFocusMode> mode{}; };
struct VideoFocusNotification final {
    std::optional<VideoFocusMode> focus{};
    std::optional<bool> unsolicited{};
};

// Borrowed body: the caller must retain the input payload. The unsigned peer
// timestamp is unscaled and unrelated to the local raw-message header clock.
struct AvSinkData final {
    std::uint64_t peer_timestamp{};
    std::span<const std::byte> body{};
};

[[nodiscard]] core::Result<AvSinkSetup> decode_av_sink_setup(std::span<const std::byte>);
// Explicit codec policy for configured channels; legacy decoding stays PCM/H.264.
[[nodiscard]] core::Result<AvSinkSetup> decode_av_sink_setup_for_codecs(
    std::span<const std::byte>, std::span<const AvSinkCodec>);
[[nodiscard]] core::Result<AvSinkStart> decode_av_sink_start(std::span<const std::byte>);
[[nodiscard]] core::Result<AvSinkStop> decode_av_sink_stop(std::span<const std::byte>);
[[nodiscard]] core::Result<VideoFocusRequest> decode_video_focus_request(std::span<const std::byte>);
[[nodiscard]] core::Result<AvSinkData> decode_av_sink_data(std::span<const std::byte>);
[[nodiscard]] core::Result<std::span<const std::byte>> decode_av_sink_codec_config(std::span<const std::byte>);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const AvSinkSetup&);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const AvSinkStart&);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const AvSinkStop&);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const VideoFocusRequest&);
[[nodiscard]] core::Result<std::vector<std::byte>> encode(const VideoFocusNotification&);
// READY carries explicit channel indices and one credit; the legacy default is index 0.
[[nodiscard]] core::Result<std::vector<std::byte>> encode_av_sink_ready(std::span<const std::uint32_t> indices = {});
// As above, advertising a credit window of `max_unacked` (>= 1) data messages.
[[nodiscard]] core::Result<std::vector<std::byte>> encode_av_sink_ready(std::span<const std::uint32_t> indices,
                                                                        std::uint32_t max_unacked);
[[nodiscard]] core::Result<std::vector<std::byte>> encode_av_sink_ack(std::int32_t session_id);

} // namespace aa::protocol
