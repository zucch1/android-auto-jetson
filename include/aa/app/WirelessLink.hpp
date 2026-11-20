// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/app/HandoffEligibility.hpp>
#include <aa/app/WiredSession.hpp>
#include <aa/transport/TcpTransport.hpp>

namespace aa::app {
class WirelessLink final : private protocol::MessageSink {
public:
    struct Config final {
        WiredSessionConfig session;
        std::vector<protocol::ServiceDescriptor> services;
        std::map<protocol::ChannelRole, protocol::MessageSink*> sinks;
        protocol::ServiceKey wifi_service{73};
        std::uint32_t ap_channel{36};
    };
    WirelessLink(WiredSessionDeps, Config);
    ~WirelessLink() override;
    core::Result<void> listen(const helper::ApSnapshot&);
    void close_admission();
    core::Result<bool> poll(HandoffEligibilityCoordinator&, std::function<void()> checkpoint = {});
    void stop();
    void set_io_checkpoint_interval(std::chrono::milliseconds interval) noexcept;
    [[nodiscard]] bool listening() const;
    [[nodiscard]] WiredSession* session() const { return session_.get(); }
    [[nodiscard]] std::optional<WiredSession::RejectedFrame> last_reject() const;
    [[nodiscard]] std::optional<diagnostics::QueueRejection> last_queue_rejection() const;
private:
    core::Result<void> on_message(const protocol::MessageView&) override;
    core::Result<void> attach(std::unique_ptr<transport::TcpTransport>, const trust::PhoneIdentity&);
    void checkpoint(HandoffEligibilityCoordinator&, const std::function<void()>&);
    void capture_session_errors();
    WiredSessionDeps deps_;
    Config config_;
    std::unique_ptr<transport::TcpListener> listener_;
    std::unique_ptr<transport::TcpTransport> transport_;
    std::unique_ptr<WiredSession> session_;
    std::string bssid_;
    std::uint64_t generation_{};
    HandoffEligibilityCoordinator* coordinator_{};
    std::chrono::steady_clock::time_point auth_until_{};
    std::chrono::milliseconds io_checkpoint_interval_{25};
    std::optional<WiredSession::RejectedFrame> last_reject_;
    std::optional<diagnostics::QueueRejection> last_queue_rejection_;
};
}
