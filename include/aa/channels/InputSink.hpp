// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/Protocol.hpp>

namespace aa::channels {

class InputSink final : public protocol::MessageSink {
public:
    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override;
    [[nodiscard]] std::uint64_t feedback() const noexcept { return feedback_; }
    [[nodiscard]] std::uint64_t unhandled() const noexcept { return unhandled_; }

private:
    std::uint64_t feedback_{0};
    std::uint64_t unhandled_{0};
};

} // namespace aa::channels
