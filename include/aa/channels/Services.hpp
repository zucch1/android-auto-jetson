// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/core/Result.hpp>
#include <aa/protocol/Protocol.hpp>

#include <map>
#include <optional>
#include <vector>

namespace aa::channels {

// Media negotiation and service dispatch (task 18 registers real handlers).
// Ownership: the registry is owned by the session and mutated only on the
// session thread; sinks are non-owning and must outlive their registration.
//
// Defaults describe preferred video sizes but advertise nothing without an
// explicitly supplied backend configuration and registered handler.
struct CapabilityProfile final {
    protocol::VideoProfile primary{};   // 1280x720@30
    protocol::VideoProfile fallback{};  // 800x480@30
    std::vector<protocol::ChannelRole> advertised{};

    [[nodiscard]] static CapabilityProfile bench_defaults();
    [[nodiscard]] bool advertises(protocol::ChannelRole role) const noexcept;
};

class Negotiator;

// Wire dispatch is private: the negotiator must first prove advertisement,
// open state and registration generation. Only diagnostics bypass wire gating.
class ServiceRegistry final {
public:
    core::Result<void> register_sink(protocol::ChannelRole role,
                                     protocol::MessageSink& sink);
    core::Result<void> unregister_sink(protocol::ChannelRole role);
    core::Result<void> dispatch_local(const protocol::MessageView& message);

    [[nodiscard]] bool has_sink(protocol::ChannelRole role) const noexcept;

private:
    friend class Negotiator;
    struct Registration final {
        protocol::MessageSink* sink;
        std::uint64_t generation;
    };
    [[nodiscard]] std::optional<std::uint64_t> generation(protocol::ChannelRole role) const;
    core::Result<void> dispatch(const protocol::MessageView& message, std::uint64_t generation);
    std::map<protocol::ChannelRole, Registration> sinks_;
    std::uint64_t next_generation_{1};
};

} // namespace aa::channels
