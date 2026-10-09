// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/channels/Services.hpp>
#include <aa/protocol/Messages.hpp>

#include <map>
#include <utility>

namespace aa::channels {

// Validated, immutable support snapshot. An unset ID requests collision-free
// allocation; an explicit ID must fit the frame byte. Only configured media
// and actual non-touch buttons have qualified wire descriptions in task 18.
class SupportConfiguration final {
public:
    SupportConfiguration() = default;
    [[nodiscard]] static core::Result<SupportConfiguration>
    create(std::vector<protocol::ServiceDescriptor> services);
    [[nodiscard]] const std::vector<protocol::ServiceDescriptor>& services() const noexcept {
        return services_;
    }
private:
    explicit SupportConfiguration(std::vector<protocol::ServiceDescriptor> services)
        : services_(std::move(services)) {}
    std::vector<protocol::ServiceDescriptor> services_;
};

// Session-thread-only; registry and registered sinks outlive this object.
// start() publishes a fresh snapshot and clears all open state. reset() must
// run on disconnect/teardown before another phone's frames can be dispatched.
// Task 20 owns lifecycle orchestration, not this channel negotiation component.
class Negotiator final {
public:
    Negotiator(SupportConfiguration support, ServiceRegistry& registry)
        : support_(std::move(support)), registry_(registry) {}
    Negotiator(const Negotiator&) = delete;
    Negotiator& operator=(const Negotiator&) = delete;
    [[nodiscard]] protocol::ServiceDiscoveryResponse start();
    void reset() noexcept;
    [[nodiscard]] core::Result<protocol::ChannelOpenResponse>
    open(const protocol::ChannelOpenRequest& request);
    core::Result<void> dispatch(const protocol::WireMessageView& message);
    [[nodiscard]] core::Result<protocol::VideoProfile>
    select_video(protocol::ServiceKey service, std::span<const protocol::VideoProfile> offered) const;
    [[nodiscard]] core::Result<protocol::AudioProfile>
    select_audio(protocol::ServiceKey service, std::span<const protocol::AudioProfile> offered) const;
private:
    struct Route final {
        protocol::ServiceDescriptor descriptor;
        std::uint64_t generation;
        bool opened{false};
    };
    [[nodiscard]] const Route* opened_route(protocol::ServiceKey service) const;
    const SupportConfiguration support_;
    ServiceRegistry& registry_;
    std::map<std::int32_t, Route> routes_;
};
} // namespace aa::channels
