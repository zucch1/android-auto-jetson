// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

namespace aa::decode_probe {

struct DecoderCandidate {
    std::string element_name;
    bool is_hardware;
    bool available;
    bool viable;
    std::string notes;
};

struct DiscoveryResult {
    std::vector<DecoderCandidate> candidates;
    std::string selected_decoder;
    bool is_hardware_selected = false;
};

DiscoveryResult discover_decoders(const std::string& forced_decoder = "");

} // namespace aa::decode_probe
