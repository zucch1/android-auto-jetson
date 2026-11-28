// SPDX-License-Identifier: GPL-3.0-or-later
// Loss-free playback-rate correction of one decoded run (PlayoutPolicy::
// lossless): the whole run is resampled onto a few frames fewer or more, so a
// backlog drains and a thin buffer refills without dropping a sample or
// inserting silence.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aa::audio::detail {

// Runs shorter than this (a third of a millisecond) are never warped; their
// correction is carried to a later run.
inline constexpr std::uint64_t kMinWarpFrames = 16;

// Largest |delta_frames| warp_frames() applies to a run of `frames`: the 1%
// rate ceiling rounded up to a whole frame, so a run of any size can take the
// frame its predecessors accumulated and none ever takes a burst of them.
[[nodiscard]] constexpr std::int64_t max_warp_frames(std::uint64_t frames) noexcept {
    return frames < kMinWarpFrames ? 0 : static_cast<std::int64_t>((frames + 99) / 100);
}

// Interleaved s16 PCM of F frames resampled onto F + delta_frames frames with
// a windowed sinc (32 taps, 8 below 64 frames) at one constant rate. The first
// and last frames (32; 16 below 256 frames, 4 below 64) are copied bit-exact
// and the warp between them starts and ends on whole samples, so a run needs no
// state from its neighbours, adds no delay and joins them without a step.
// delta_frames == 0, or beyond max_warp_frames(), returns the input unchanged.
[[nodiscard]] std::vector<std::byte> warp_frames(std::span<const std::byte> pcm,
                                                 std::size_t channels,
                                                 std::int64_t delta_frames);

} // namespace aa::audio::detail
