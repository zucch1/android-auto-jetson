// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Access-unit split with the exact acceptance rules of tools/aa-decode-probe
// (the same copy the consumer_decode and dummy_display suites feed), so the
// replay stream this test-only receiver serves matches the established
// fixture grammar.

#include <cstdint>
#include <vector>

namespace aatestreceiver {

struct AnnexBUnit final {
    std::vector<std::uint8_t> data{};
    bool idr{false};
};

inline std::vector<AnnexBUnit> parse_annexb_units(const std::vector<std::uint8_t>& stream) {
    std::vector<AnnexBUnit> units;
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i + 3 < stream.size(); ++i) {
        if (stream[i] == 0 && stream[i + 1] == 0) {
            if (stream[i + 2] == 1) {
                starts.push_back(i);
                i += 2;
            } else if (stream[i + 2] == 0 && i + 3 < stream.size() && stream[i + 3] == 1) {
                starts.push_back(i);
                i += 3;
            }
        }
    }
    if (starts.empty()) {
        return units;
    }
    std::vector<std::uint8_t> current;
    bool current_has_idr = false;
    bool au_has_slice = false;
    bool has_aud = false;
    for (std::size_t idx = 0; idx < starts.size(); ++idx) {
        const std::size_t start = starts[idx];
        const std::size_t end = (idx + 1 < starts.size()) ? starts[idx + 1] : stream.size();
        const std::size_t nal_offset = start + (stream[start + 2] == 1 ? 3 : 4);
        if (nal_offset >= stream.size()) {
            continue;
        }
        const std::uint8_t nal_type = static_cast<std::uint8_t>(stream[nal_offset] & 0x1F);
        if (nal_type == 9) {
            has_aud = true;
        }
        if ((nal_type == 1 || nal_type == 5) && !has_aud) {
            return {};
        }
        if (nal_type == 9 || ((nal_type == 7 || nal_type == 8) && au_has_slice)) {
            if (au_has_slice) {
                units.push_back({std::move(current), current_has_idr});
                current.clear();
                current_has_idr = false;
                au_has_slice = false;
            }
        }
        current.insert(current.end(), stream.begin() + static_cast<std::ptrdiff_t>(start),
                       stream.begin() + static_cast<std::ptrdiff_t>(end));
        if (nal_type == 5) {
            current_has_idr = true;
            au_has_slice = true;
        } else if (nal_type == 1) {
            au_has_slice = true;
        }
    }
    if (au_has_slice) {
        units.push_back({std::move(current), current_has_idr});
    }
    return units;
}

} // namespace aatestreceiver
