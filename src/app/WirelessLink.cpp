// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WirelessLink.hpp>
#include <aa/protocol/WifiProjection.hpp>
#include <aa/transport/TlsTransport.hpp>
#include <arpa/inet.h>

namespace aa::app {
using namespace std::chrono_literals;
WirelessLink::WirelessLink(WiredSessionDeps deps, Config config) : deps_(deps), config_(std::move(config)) {}
WirelessLink::~WirelessLink() { stop(); }
bool WirelessLink::listening() const { return listener_ != nullptr; }
core::Result<void> WirelessLink::listen(const helper::ApSnapshot& snapshot) {
    in_addr address{};
    if (::inet_pton(AF_INET, snapshot.ip.c_str(), &address) != 1 || address.s_addr == INADDR_ANY ||
        IN_MULTICAST(ntohl(address.s_addr)) || snapshot.port < 1024 || snapshot.port > 65535 || !snapshot.generation)
        return Error{ErrorCode::invalid_argument};
    close_admission();
    auto listener = transport::TcpListener::listen(snapshot.ip.c_str(), static_cast<std::uint16_t>(snapshot.port));
    if (!listener) return listener.error();
    bssid_ = snapshot.bssid;
    listener_ = std::move(listener).value();
    return {};
}
void WirelessLink::close_admission() {
    if (listener_) listener_->close();
    listener_.reset();
}
void WirelessLink::stop() {
    close_admission();
    if (session_) {
        session_->request_stop(); session_->tick();
        if (session_->state() == session::State::stopping) (void)session_->apply(session::Event::cleanup_finished);
    }
    if (transport_) { transport_->set_io_checkpoint({}); transport_->close(); }
    session_.reset(); transport_.reset(); coordinator_ = nullptr;
}
core::Result<void> WirelessLink::attach(std::unique_ptr<transport::TcpTransport> tcp,
                                       const trust::PhoneIdentity& phone) {
    auto config = config_.session;
    config.lifecycle.link = session::LinkKind::wireless;
    config.lifecycle.phases.negotiate = core::Milliseconds{10'000};
    session_ = std::make_unique<WiredSession>(deps_, config);
    auto services = config_.services;
    for (const auto& descriptor : services)
        if (descriptor.role == protocol::ChannelRole::wifi_projection) return Error{ErrorCode::invalid_argument};
    services.push_back({config_.wifi_service, protocol::ChannelRole::wifi_projection,
                       protocol::WifiProjectionConfiguration{bssid_}});
    if (auto configured = session_->configure(std::move(services)); !configured) return configured.error();
    for (const auto& [role, sink] : config_.sinks) {
        if (!sink) return Error{ErrorCode::invalid_argument};
        if (auto registered = session_->register_sink(role, *sink); !registered) return registered.error();
    }
    if (auto registered = session_->register_sink(protocol::ChannelRole::wifi_projection, *this); !registered)
        return registered.error();
    transport_ = std::move(tcp);
    session_->attach_transport(transport_.get());
    auto cipher = transport::make_client_handshake_cipher([this, phone] {
        return coordinator_ && coordinator_->valid_session() &&
            (session_->authenticated() || coordinator_->admitting()) &&
            deps_.trust.lookup(phone) == trust::Decision::approved;
    });
    if (!cipher) return Error{ErrorCode::transport_unauthorized_peer};
    session_->attach_cipher(std::move(cipher));
    if (auto result = session_->apply(session::Event::start_discovery); !result) return result.error();
    if (auto result = session_->phone_discovered(phone); !result) return result.error();
    if (auto result = session_->apply(session::Event::transport_ready); !result) return result.error();
    auth_until_ = std::chrono::steady_clock::now() + 9950ms;
    return {};
}
void WirelessLink::checkpoint(HandoffEligibilityCoordinator& coordinator, const std::function<void()>& work) {
    if (work) work();
    const bool active = session_ && (session_->state() == session::State::active || session_->state() == session::State::degraded);
    coordinator.tick(active);
    if (!session_) return;
    if (!coordinator.valid_session() || (!session_->authenticated() && std::chrono::steady_clock::now() >= auth_until_))
        session_->request_stop();
    if (session_->state() == session::State::active && !coordinator.admitting())
        (void)session_->apply(session::Event::degrade);
}
core::Result<bool> WirelessLink::poll(HandoffEligibilityCoordinator& coordinator, std::function<void()> work) {
    coordinator_ = &coordinator;
    checkpoint(coordinator, work);
    if (!session_) {
        if (!listener_ || !coordinator.admitting()) return false;
        // The checkpoint can retire admission, but cannot destroy the listener
        // whose accept is on-stack. Retirement is applied after accept returns.
        auto listener = std::move(listener_);
        core::CancellationSource cancel;
        auto tcp = listener->accept({std::chrono::steady_clock::now() + 25ms, cancel.get_token()}, [&] {
            checkpoint(coordinator, work);
            if (!coordinator.admitting()) cancel.request_stop();
        });
        if (coordinator.admitting()) listener_ = std::move(listener);
        if (!tcp) {
            if (tcp.error().code() == ErrorCode::timeout || tcp.error().code() == ErrorCode::cancelled) return false;
            return tcp.error();
        }
        const auto phone = coordinator.phone();
        bool attested = false;
#ifdef AA_WIRELESS_TEST_ATTESTATION
        attested = phone && attestation_ && attestation_(*tcp.value(), *phone);
#endif
        if (!attested || !phone || !coordinator.admitting()) return Error{ErrorCode::transport_unauthorized_peer};
        if (auto attached = attach(std::move(tcp).value(), *phone); !attached) { stop(); coordinator.retire(); return attached.error(); }
        close_admission();
        transport_->set_io_checkpoint([this, &coordinator, work] { checkpoint(coordinator, work); });
        if (auto sent = session_->send_version_request(1, 6); !sent) { stop(); coordinator.retire(); return sent.error(); }
        return true;
    }
    auto result = session_->poll_transport();
    session_->tick();
    if (!result || session_->token().stop_requested() || !coordinator.valid_session()) {
        const auto error = result ? Error{ErrorCode::cancelled} : result.error();
        stop(); coordinator.retire(); return error;
    }
    if (session_->authenticated() && session_->channel_opens() && session_->state() == session::State::negotiating) {
        if (coordinator.bootstrap_completed()) (void)session_->apply(session::Event::negotiation_succeeded);
        else coordinator.confirm_completion();
    }
    return result;
}
core::Result<void> WirelessLink::on_message(const protocol::MessageView& message) {
    if (!coordinator_ || !coordinator_->admitting()) return Error{ErrorCode::transport_unauthorized_peer};
    auto request = protocol::decode_wifi_credentials_request(message);
    if (!request) return request.error();
    auto payload = protocol::encode(protocol::WifiCredentialsResponse{
        protocol::WifiSecurityMode::wpa2_personal, {36}, protocol::WifiAccessPointType::dynamic});
    if (!payload) return payload.error();
    return session_->send_channel_payload(protocol::ChannelRole::wifi_projection, std::move(payload).value());
}
}
