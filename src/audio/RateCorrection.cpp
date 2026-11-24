// SPDX-License-Identifier: GPL-3.0-or-later
#include "RateCorrection.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace aa::audio::detail {
namespace {

constexpr std::size_t kPhases = 256;
constexpr std::size_t kLongTaps = 32;
constexpr std::size_t kShortTaps = 8;

template <std::size_t Taps>
using Kernel = std::array<std::array<float, Taps>, kPhases + 1>;

// Frames copied bit-exact at each end; at least half the taps, so every kernel
// stays inside the run.
[[nodiscard]] constexpr std::size_t edge_frames(std::size_t frames) noexcept {
    return frames >= 256 ? 32 : frames >= 64 ? kLongTaps / 2 : kShortTaps / 2;
}

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
template <std::size_t Taps>
[[nodiscard]] const Kernel<Taps>& kernel() {
    static const Kernel<Taps> table = [] {
        constexpr double kBeta = 8.0;
        constexpr double kHalf = static_cast<double>(Taps) / 2.0;
        Kernel<Taps> out{};
        for (std::size_t phase = 0; phase <= kPhases; ++phase) {
            const double fraction = static_cast<double>(phase) / static_cast<double>(kPhases);
            std::array<double, Taps> row{};
            double sum = 0.0;
            for (std::size_t tap = 0; tap < Taps; ++tap) {
                // Distance from the interpolated position to this sample.
                const double x = static_cast<double>(tap) - (kHalf - 1.0) - fraction;
                const double sinc = x == 0.0
                    ? 1.0 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                const double ratio = std::min(1.0, std::abs(x) / kHalf);
                row[tap] = sinc * bessel_i0(kBeta * std::sqrt(1.0 - ratio * ratio)) / bessel_i0(kBeta);
                sum += row[tap];
            }
            for (std::size_t tap = 0; tap < Taps; ++tap) {
                out[phase][tap] = static_cast<float>(row[tap] / sum);
            }
        }
        return out;
    }();
    return table;
}

// Resamples in[edge, frames - edge) onto out[edge, out_frames - edge).
template <std::size_t Taps>
void warp_middle(const std::vector<std::int16_t>& in, std::vector<std::byte>& out,
                 std::size_t channels, std::size_t frames, std::size_t out_frames) {
    const auto edge = edge_frames(frames);
    const auto& table = kernel<Taps>();
    const double step = static_cast<double>(frames - 2 * edge)
        / static_cast<double>(out_frames - 2 * edge);
    for (std::size_t i = edge; i < out_frames - edge; ++i) {
        const double position = static_cast<double>(edge) + static_cast<double>(i - edge) * step;
        const auto whole = std::min(static_cast<std::size_t>(position), frames - edge - 1);
        const double phase = (position - static_cast<double>(whole)) * static_cast<double>(kPhases);
        const auto row = std::min(static_cast<std::size_t>(phase), kPhases - 1);
        const auto blend = static_cast<float>(phase - static_cast<double>(row));
        for (std::size_t channel = 0; channel < channels; ++channel) {
            float sum = 0.0F;
            for (std::size_t tap = 0; tap < Taps; ++tap) {
                const auto sample = in[(whole + tap - (Taps / 2 - 1)) * channels + channel];
                const auto weight = table[row][tap] + blend * (table[row + 1][tap] - table[row][tap]);
                sum += weight * static_cast<float>(sample);
            }
            const auto value = static_cast<std::int16_t>(
                std::clamp(std::lround(sum), -32768L, 32767L));
            std::memcpy(out.data() + (i * channels + channel) * sizeof(value), &value, sizeof(value));
        }
    }
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
    const auto edge = edge_frames(frames);
    copy(0, 0, edge);
    copy(out_frames - edge, frames - edge, edge);
    if (frames >= 64) { warp_middle<kLongTaps>(in, out, channels, frames, out_frames); }
    else { warp_middle<kShortTaps>(in, out, channels, frames, out_frames); }
    return out;
}

} // namespace aa::audio::detail
