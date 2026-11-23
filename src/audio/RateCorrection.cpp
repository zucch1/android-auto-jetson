// SPDX-License-Identifier: GPL-3.0-or-later
#include "RateCorrection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace aa::audio::detail {
namespace {

constexpr std::size_t kTaps = 32;
constexpr std::size_t kPhases = 256;
// Copied bit-exact at each end; also keeps every kernel inside the run.
constexpr std::size_t kEdge = kTaps;

using Kernel = std::array<std::array<float, kTaps>, kPhases + 1>;

[[nodiscard]] double bessel_i0(double x) {
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

// Kaiser (beta 8) windowed sinc, one row per sub-sample phase, each row
// normalized to unity gain so a constant signal stays exactly constant.
[[nodiscard]] const Kernel& kernel() {
    static const Kernel table = [] {
        constexpr double kBeta = 8.0;
        constexpr double kHalf = static_cast<double>(kTaps) / 2.0;
        Kernel out{};
        for (std::size_t phase = 0; phase <= kPhases; ++phase) {
            const double fraction = static_cast<double>(phase) / static_cast<double>(kPhases);
            std::array<double, kTaps> row{};
            double sum = 0.0;
            for (std::size_t tap = 0; tap < kTaps; ++tap) {
                // Distance from the interpolated position to this sample.
                const double x = static_cast<double>(tap) - (kHalf - 1.0) - fraction;
                const double sinc = x == 0.0
                    ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                const double ratio = std::min(1.0, std::abs(x) / kHalf);
                row[tap] = sinc * bessel_i0(kBeta * std::sqrt(1.0 - ratio * ratio)) / bessel_i0(kBeta);
                sum += row[tap];
            }
            for (std::size_t tap = 0; tap < kTaps; ++tap) {
                out[phase][tap] = static_cast<float>(row[tap] / sum);
            }
        }
        return out;
    }();
    return table;
}

} // namespace

std::vector<std::byte> warp_frames(std::span<const std::byte> pcm, std::size_t channels,
                                   std::int64_t delta_frames) {
    std::vector<std::byte> unchanged(pcm.begin(), pcm.end());
    const auto frame_bytes = channels * sizeof(std::int16_t);
    if (frame_bytes == 0 || pcm.size() % frame_bytes != 0) { return unchanged; }
    const auto frames = pcm.size() / frame_bytes;
    if (delta_frames == 0 || std::abs(delta_frames) > max_warp_frames(frames)) { return unchanged; }

    std::vector<std::int16_t> in(frames * channels);
    std::memcpy(in.data(), pcm.data(), pcm.size());
    const auto out_frames = static_cast<std::size_t>(static_cast<std::int64_t>(frames) + delta_frames);
    std::vector<std::byte> out(out_frames * frame_bytes);
    const auto copy = [&](std::size_t to, std::size_t from, std::size_t count) {
        std::memcpy(out.data() + to * frame_bytes, pcm.data() + from * frame_bytes, count * frame_bytes);
    };
    copy(0, 0, kEdge);
    copy(out_frames - kEdge, frames - kEdge, kEdge);

    const auto& table = kernel();
    const double step = static_cast<double>(frames - 2 * kEdge)
        / static_cast<double>(out_frames - 2 * kEdge);
    for (std::size_t i = kEdge; i < out_frames - kEdge; ++i) {
        const double position = static_cast<double>(kEdge) + static_cast<double>(i - kEdge) * step;
        const auto whole = std::min(static_cast<std::size_t>(position), frames - kEdge - 1);
        const double phase = (position - static_cast<double>(whole)) * static_cast<double>(kPhases);
        const auto row = std::min(static_cast<std::size_t>(phase), kPhases - 1);
        const auto blend = static_cast<float>(phase - static_cast<double>(row));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            float sum = 0.0F;
            for (std::size_t tap = 0; tap < kTaps; ++tap) {
                const auto sample = in[(whole + tap - (kTaps / 2 - 1)) * channels + channel];
                const auto weight = table[row][tap] + blend * (table[row + 1][tap] - table[row][tap]);
                sum += weight * static_cast<float>(sample);
            }
            const auto value = static_cast<std::int16_t>(
                std::clamp(std::lround(sum), -32768L, 32767L));
            std::memcpy(out.data() + (i * channels + channel) * sizeof(value), &value, sizeof(value));
        }
    }
    return out;
}

} // namespace aa::audio::detail
