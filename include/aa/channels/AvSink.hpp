// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/AvSink.hpp>
#include <aa/protocol/Messages.hpp>

#include <functional>
#include <utility>

namespace aa::channels {

class AvSink final : public protocol::MessageSink {
public:
    using Send = std::function<core::Result<void>(std::vector<std::byte>)>;

    AvSink(protocol::ServiceDescriptor service, protocol::MessageSink& raw, Send send)
        : service_(std::move(service)), raw_(raw), send_(std::move(send)) {}

    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override;
    void reset() noexcept;
    [[nodiscard]] bool input_focus_allowed() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> peer_timestamp() const noexcept { return peer_timestamp_; }

private:
    [[nodiscard]] core::Result<void> send(core::Result<std::vector<std::byte>> payload);
    [[nodiscard]] core::Result<void> setup(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> start(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> codec_config(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> data(const protocol::MessageView& message);
    [[nodiscard]] core::Result<void> focus(std::span<const std::byte> payload);

    protocol::ServiceDescriptor service_;
    protocol::MessageSink& raw_;
    Send send_;
    bool ready_{};
    std::optional<std::int32_t> session_;
    std::optional<std::uint64_t> peer_timestamp_;
    std::vector<std::byte> codec_cache_;
    bool staged_credit_{};
    protocol::VideoFocusMode focus_{protocol::VideoFocusMode::projected};
};

} // namespace aa::channels
