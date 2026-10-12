// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/audio/Audio.hpp>
#include <aa/core/Result.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aa::audio {

// AAP audio decode seam (task 25): turns channel/session audio payloads into
// normalized PcmChunk values for the mixer. Codec kinds mirror the AASDK
// MediaCodecType audio values (PCM=1, AAC_LC=2, AAC_LC_ADTS=4); raw AAC-LC is
// only decodable when the AudioSpecificConfig travels in
// EncodedChunk::codec_data, otherwise the decoder is disabled with a recorded
// cause (honest backend policy, never a silent wrong-format decode).
enum class Codec : std::uint8_t { pcm, aac_lc, aac_lc_adts };

[[nodiscard]] std::string_view to_string(Codec codec) noexcept;

// One payload as delivered by the channel/session audio path (task-18
// dispatch shape: role, sequence, common-monotonic timestamp, payload). The
// spans are valid only for the duration of the decode() call.
struct EncodedChunk final {
    Role role{};
    Codec codec{Codec::pcm};
    AudioFormat input_format{};
    std::uint64_t sequence{};
    core::Nanoseconds timestamp{};
    std::span<const std::byte> payload{};
    std::span<const std::byte> codec_data{};
};

// Backend honesty (task-23 probe pattern): every known element is reported
// with availability/viability and a recorded cause; absence is a recorded
// fact, never an assumption.
struct DecodeCandidate final {
    std::string element{};
    bool available{};
    bool viable{};
    std::string cause{};
};

struct DecodeReport final {
    Codec codec{};
    std::vector<DecodeCandidate> candidates{};
    std::string selected_element{};
    bool usable{};
    std::string cause{};
};

// Pure selection policy over a probe report (task-25 gate fix F5, mirrors
// select_sink_backend): chooses the first viable AAC decoder alternative in
// priority order and requires the normalization chain (plus the AAC parser for
// non-PCM codecs). Every non-viable candidate is named in the recorded "disabled:"
// cause - absence is a recorded fact, never an assumption. Fixture-testable
// without any host installation state.
struct DecodeSelection final {
    std::string element{};
    bool usable{};
    std::string cause{};
};

[[nodiscard]] DecodeSelection select_decode_backend(const DecodeReport& report);

// Per-role decoder. Caller-driven: decode() runs the GStreamer pipeline on the
// calling thread (push one payload, pull one normalized chunk). The returned
// PcmChunk::pcm span points into decoder-owned storage valid until the next
// decode()/close() call.
class AudioDecoder final {
public:
    struct Config final {
        Codec codec{Codec::pcm};
        AudioFormat input_format{};
        AudioFormat output_format{};
    };

    [[nodiscard]] static DecodeReport probe(Codec codec);
    [[nodiscard]] static core::Result<AudioDecoder> open(const Config& config);

    AudioDecoder(AudioDecoder&&) noexcept;
    AudioDecoder& operator=(AudioDecoder&&) noexcept;
    ~AudioDecoder();

    // Normalizes one payload to config.output_format and returns every PCM
    // frame the payload produced, concatenated in order (an AAC payload may
    // carry several coded units and is drained until the parser goes quiet; a
    // PCM payload is strictly 1:1 and returns without waiting). Malformed
    // payload -> audio_decode_failed; missing decode element for the codec ->
    // audio_format_unsupported (with the recorded cause in the report).
    [[nodiscard]] core::Result<PcmChunk> decode(const EncodedChunk& chunk);
    void flush() noexcept;
    void close() noexcept;

    [[nodiscard]] const DecodeReport& report() const noexcept;

    struct Impl;
    explicit AudioDecoder(std::unique_ptr<Impl> impl) noexcept;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace aa::audio
