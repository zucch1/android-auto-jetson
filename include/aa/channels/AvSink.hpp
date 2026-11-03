// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/protocol/AvSink.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/diagnostics/QueueRejection.hpp>
#include <aa/channels/AvObservation.hpp>

#include <functional>
#include <utility>

namespace aa::channels {

struct DeferredAvDelivery final {
    // The view is borrowed: submit MUST copy it before returning. Success only
    // accepts work; completion must be posted back to the owner thread later.
    // Neither callback may reenter the sink; invalidate must not throw.
    std::function<core::Result<void>(const protocol::MessageView&, AvDeliveryId)> submit;
    std::function<void(protocol::ChannelRole, std::uint64_t new_generation)> invalidate;
    // Read only immediately after submit fails with ipc_queue_full. Optional:
    // an uninstrumented downstream failure must remain unattributed.
    std::function<std::optional<diagnostics::QueueRejection>()> rejection{};
    // Observational only: must not throw, reenter the sink, or perform transport I/O.
    std::function<void(const AvObservation&)> observe{};
};

class AvSink final : public protocol::MessageSink {
public:
    using Send = std::function<core::Result<void>(std::vector<std::byte>)>;

    AvSink(protocol::ServiceDescriptor service, protocol::MessageSink& raw, Send send)
        : service_(std::move(service)), raw_(&raw), send_(std::move(send)) {}

    AvSink(protocol::ServiceDescriptor service, DeferredAvDelivery delivery, Send send)
        : service_(std::move(service)), deferred_(std::move(delivery)), send_(std::move(send)) {}

    // A session-owned counter must outlive this sink and never be reset. Sharing
    // it across replacement sinks prevents a retired sink's IDs being reused.
    AvSink(protocol::ServiceDescriptor service, DeferredAvDelivery delivery, Send send,
           std::uint64_t& generation_counter)
        : AvSink(std::move(service), std::move(delivery), std::move(send)) {
        generation_counter_ = &generation_counter;
        generation_ = generation_counter;
    }

    [[nodiscard]] core::Result<void> on_message(const protocol::MessageView& message) override;
    // Owner-thread only, after submit returns. Stale/duplicate IDs are ignored,
    // including stale failures; the production owner must latch worker failures
    // independently even when their delivery has already been invalidated.
    [[nodiscard]] core::Result<void> complete_av_delivery(AvDeliveryId id, core::Result<void> result);
    void reset() noexcept;
    void observe_receive(bool entering) const;
    [[nodiscard]] bool input_focus_allowed() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> peer_timestamp() const noexcept { return peer_timestamp_; }
    [[nodiscard]] std::optional<diagnostics::QueueRejection> last_queue_rejection() const noexcept {
        return last_queue_rejection_;
    }

private:
    enum class AckFor : std::uint16_t { data = 0, codec_config = 1 };
    void observe(AvCreditEvent event, std::optional<ErrorCode> error = {}, std::uint32_t count = 0) const;
    [[nodiscard]] core::Result<void> send_ack(std::int32_t session, AckFor for_message);
    void invalidate_delivery() noexcept;
    [[nodiscard]] core::Result<void> send(core::Result<std::vector<std::byte>> payload);
    [[nodiscard]] core::Result<void> setup(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> start(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> codec_config(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> data(const protocol::MessageView& message);
    [[nodiscard]] core::Result<void> focus(std::span<const std::byte> payload);

    protocol::ServiceDescriptor service_;
    protocol::MessageSink* raw_{};
    std::optional<DeferredAvDelivery> deferred_;
    Send send_;
    std::uint64_t generation_{};
    std::uint64_t* generation_counter_{};
    std::uint64_t delivery_sequence_{};
    std::optional<AvDeliveryId> pending_;
    AvObservation current_observation_{};
    AvObservation pending_observation_{};
    std::optional<diagnostics::QueueRejection> last_queue_rejection_;
    bool ready_{};
    std::optional<std::int32_t> session_;
    std::optional<std::uint64_t> peer_timestamp_;
    std::vector<std::byte> codec_cache_;
    bool staged_credit_{};
    protocol::VideoFocusMode focus_{protocol::VideoFocusMode::projected};
};

} // namespace aa::channels
