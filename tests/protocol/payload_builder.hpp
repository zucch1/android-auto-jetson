// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test-only wire fixture: builds control payloads with the pinned SDK's own
// MessageId framing so round-trip tests exercise the adapter against the real
// byte order instead of a test re-implementation.

#include <aasdk/Messenger/MessageId.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace testwire {

[[nodiscard]] inline std::vector<std::byte> control_payload(std::uint16_t raw_id,
                                                            const std::string& body) {
    const aasdk::messenger::MessageId id{raw_id};
    const aasdk::common::Data prefix = id.getData();
    std::vector<std::byte> payload;
    payload.reserve(prefix.size() + body.size());
    for (const auto byte : prefix) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    for (const char byte : body) {
        payload.push_back(static_cast<std::byte>(byte));
    }
    return payload;
}

} // namespace testwire
