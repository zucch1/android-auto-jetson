// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/Protocol.hpp>

#include <functional>
#include <utility>
#include <vector>

namespace aa::channels {

// Session-thread-only negotiation endpoint. No capture backend or stream state.
class MicrophoneSink final : public protocol::MessageSink {
public:
    using Send = std::function<core::Result<void>(std::vector<std::byte>)>;
    explicit MicrophoneSink(Send send) : send_(std::move(send)) {}
    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override;

private:
    Send send_;
};

} // namespace aa::channels
