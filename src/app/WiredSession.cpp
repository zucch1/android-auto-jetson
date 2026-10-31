// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WiredSession.hpp>
#include <aa/core/Deadline.hpp>

#include <aa/protocol/InputReport.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace aa::app {
namespace {

constexpr Error not_configured() { return Error{ErrorCode::channel_not_registered}; }
constexpr Error bad_argument() { return Error{ErrorCode::invalid_argument}; }

} // namespace

// Receiver-side forwarding of one normalized input event (task 26's
// InputChannelTarget seam). The payload reuses the project's bounded input
// record codec (ipc::encode_input_record); it is framed on the negotiated input
// service channel and sent over the session transport.
class WiredSession::InputForwarder final : public input::InputChannelTarget {
  public:
    explicit InputForwarder(WiredSession &owner) noexcept : owner_(owner) {}
    core::Result<void> on_normalized(const input::NormalizedInput &event) override {
        return owner_.forward_input(event);
    }

  private:
    WiredSession &owner_;
};

WiredSession::WiredSession(WiredSessionDeps deps, WiredSessionConfig config)
    : deps_(deps), config_(std::move(config)),
      lifecycle_(config_.lifecycle, deps_.clock, deps_.trust, nullptr),
      session_id_(config_.session), last_state_(lifecycle_.state()) {
    if (config_.input_enabled) {
        input_forwarder_ = std::make_unique<InputForwarder>(*this);
        input_service_.emplace(config_.input, *input_forwarder_);
    }
}

WiredSession::~WiredSession() { reset_owned_av(); }

// ---------------------------------------------------------------------------
// Session / trust delegation. No approval happens here: the store decides.
// ---------------------------------------------------------------------------

core::Result<void> WiredSession::apply(session::Event event) {
    auto outcome = lifecycle_.apply(event);
    // Recovery telemetry resolves BEFORE the state-change record: an event that
    // both ends a recovery attempt and moves the state (a failed reconnect)
    // must produce exactly one terminal recovery record, not one per hook.
    note_recovery(event, outcome);
    note_state();
    return outcome;
}

core::Result<void> WiredSession::phone_discovered(const trust::PhoneIdentity &identity) {
    auto outcome = lifecycle_.phone_discovered(identity);
    if (outcome.has_value()) {
        // Remember the ADMITTED identity for recovery-telemetry attribution
        // (item 3): a rejected discovery never re-labels the admitted phone.
        last_identity_key_ = identity.key;
    }
    note_state(identity.key);
    return outcome;
}

core::Result<void> WiredSession::fail(Error reason) {
    auto outcome = lifecycle_.fail(reason);
    note_state();
    return outcome;
}

void WiredSession::tick() {
    lifecycle_.tick();
    // Deadline expiry is an owner-thread transition like any other: surface the
    // state change and resolve any open recovery as terminal failed (item 1).
    note_state();
}

void WiredSession::request_stop() noexcept { lifecycle_.request_stop(); }

core::Deadline WiredSession::io_deadline(bool writing) const noexcept {
    auto until = writing ? core::Deadline::Clock::now() + std::chrono::seconds{5}
                         : core::Deadline::Clock::time_point::max();
    if (const auto left = lifecycle_.remaining_budget()) {
        until = std::min(until, core::Deadline::Clock::now() +
                                   std::chrono::nanoseconds{left->count});
    }
    return {until, token()};
}

Error WiredSession::io_error(Error reason, const core::Deadline &deadline) {
    tick();
    if (failure()) {
        return *failure();
    }
    if (auto checked = deadline.check(); !checked) {
        return checked.error();
    }
    return reason;
}

core::Result<void> WiredSession::send_frame(std::span<const std::byte> frame) {
    if (transport_ == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    const auto deadline = io_deadline(true);
    if (auto checked = deadline.check(); !checked) {
        return io_error(checked.error(), deadline);
    }
    core::DeadlineCancellation bounded{deadline};
    transport_->set_io_deadline(deadline.until);
    if (auto opened = transport_->open(bounded.token()); !opened) {
        return io_error(opened.error(), deadline);
    }
    if (auto sent = transport_->send(frame); !sent) {
        return io_error(sent.error(), deadline);
    }
    if (auto checked = deadline.check(); !checked) {
        return io_error(checked.error(), deadline);
    }
    return {};
}

void WiredSession::attach_transport(transport::Transport *transport) noexcept {
    transport_ = transport;
    lifecycle_.attach_transport(transport);
    reset_link();
}

// Per-link lifecycle (finding P1-B): a fresh transport is a fresh link (the
// lifecycle never reuses a retired handle), so every byte of the previous wire
// stream, all negotiated channel-open state and the previous link's cipher are
// dropped. Reusing the retired link's cipher would let stale negotiated keys
// authenticate or seal on the new link without any handshake; sealing stays
// impossible until attach_cipher() binds a fresh cipher and its handshake
// completes (channels::Negotiator::reset() contract).
void WiredSession::reset_link() noexcept {
    reset_owned_av();
    parser_ = transport::FrameStreamParser{};
    reassembler_ = transport::MessageReassembler{};
    if (negotiator_.has_value()) {
        negotiator_->reset();
    }
    auth_phase_ = AuthPhase::awaiting_version;
    ssl_active_ = false;
    cipher_.reset();
    sequence_ = 0;
    channel_opens_ = 0;
    unhandled_controls_ = 0;
    channel_closes_ = 0;
    last_reject_.reset();
    last_queue_rejection_.reset();
    version_responses_ = 0;
    pings_answered_ = 0;
    encapsulated_ssls_ = 0;
    auth_completes_ = 0;
}

void WiredSession::attach_cipher(std::unique_ptr<transport::HandshakingCipher> cipher) noexcept {
    reset_owned_av();
    cipher_ = std::move(cipher);
    // P1-C: the same negotiated cipher opens inbound E=1 records. Bind it into
    // the reassembler at every cipher attach (initial link and each reconnect);
    // inbound encrypted frames still fail closed until the handshake completes
    // (the reassembler requires an active cipher).
    reassembler_ = transport::MessageReassembler{cipher_.get()};
}

// ---------------------------------------------------------------------------
// Channel services.
// ---------------------------------------------------------------------------

core::Result<void> WiredSession::configure(std::vector<protocol::ServiceDescriptor> services) {
    if (negotiator_.has_value()) {
        return bad_argument();
    }
    auto support = channels::SupportConfiguration::create(std::move(services));
    if (!support) {
        return support.error();
    }
    descriptors_ = support.value().services();
    negotiator_.emplace(std::move(support).value(), registry_);
    // Register any sinks supplied before the descriptors were known.
    for (auto &[role, sink] : sinks_) {
        if (auto registered = registry_.register_sink(role, *sink); !registered) {
            return registered.error();
        }
    }
    return {};
}

core::Result<void> WiredSession::register_sink(protocol::ChannelRole role,
                                               protocol::MessageSink &sink) {
    if (sinks_.contains(role)) {
        return bad_argument();
    }
    sinks_.emplace(role, &sink);
    if (negotiator_.has_value()) {
        return registry_.register_sink(role, sink);
    }
    return {};
}

core::Result<void> WiredSession::register_av_sink(protocol::ChannelRole role,
                                                  protocol::MessageSink &raw_sink) {
    if (!negotiator_ || sinks_.contains(role)) { return bad_argument(); }
    switch (role) {
    case protocol::ChannelRole::video:
    case protocol::ChannelRole::media_audio:
    case protocol::ChannelRole::guidance_audio:
    case protocol::ChannelRole::system_audio:
    case protocol::ChannelRole::speech_audio: break;
    default: return Error{ErrorCode::protocol_unsupported_channel};
    }
    const auto service = std::find_if(descriptors_.begin(), descriptors_.end(),
        [role](const auto &descriptor) { return descriptor.role == role; });
    if (service == descriptors_.end()) { return not_configured(); }
    auto wrapper = std::make_unique<channels::AvSink>(*service, raw_sink,
        [this, role](std::vector<std::byte> payload) {
            return send_channel_payload(role, std::move(payload));
        });
    if (auto registered = registry_.register_sink(role, *wrapper); !registered) {
        return registered.error();
    }
    sinks_.emplace(role, wrapper.get());
    av_sinks_.emplace(role, std::move(wrapper));
    return {};
}

core::Result<void> WiredSession::register_deferred_av_sink(protocol::ChannelRole role,
                                                           channels::DeferredAvDelivery delivery) {
    if (!negotiator_ || sinks_.contains(role) || !delivery.submit) { return bad_argument(); }
    switch (role) {
    case protocol::ChannelRole::video:
    case protocol::ChannelRole::media_audio:
    case protocol::ChannelRole::guidance_audio:
    case protocol::ChannelRole::system_audio:
    case protocol::ChannelRole::speech_audio: break;
    default: return Error{ErrorCode::protocol_unsupported_channel};
    }
    const auto service = std::find_if(descriptors_.begin(), descriptors_.end(),
        [role](const auto &descriptor) { return descriptor.role == role; });
    if (service == descriptors_.end()) { return not_configured(); }
    auto wrapper = std::make_unique<channels::AvSink>(*service, std::move(delivery),
        [this, role](std::vector<std::byte> payload) {
            return send_channel_payload(role, std::move(payload));
        }, av_generation_);
    if (auto registered = registry_.register_sink(role, *wrapper); !registered) {
        return registered.error();
    }
    sinks_.emplace(role, wrapper.get());
    av_sinks_.emplace(role, std::move(wrapper));
    return {};
}

core::Result<void> WiredSession::complete_av_delivery(channels::AvDeliveryId id,
                                                      core::Result<void> result) {
    const auto sink = av_sinks_.find(id.role);
    if (sink == av_sinks_.end()) { return {}; }
    return sink->second->complete_av_delivery(id, result);
}

void WiredSession::reset_owned_av() noexcept {
    for (auto &[role, sink] : av_sinks_) { sink->reset(); }
}

core::Result<std::size_t> WiredSession::announce_services() {
    if (!negotiator_.has_value()) {
        return not_configured();
    }
    reset_owned_av();
    return send_payload(negotiator_->start());
}

core::Result<std::size_t> WiredSession::send_version_request(std::uint16_t major,
                                                             std::uint16_t minor) {
    auto payload = protocol::encode(protocol::VersionRequest{major, minor});
    if (!payload) {
        return payload.error();
    }
    transport::Message message{std::uint8_t{0}, transport::MessageKind::specific,
                               std::move(payload).value()};
    auto frames = transport::encode_message(message, transport::Encryption::plain, nullptr);
    if (!frames) {
        return frames.error();
    }
    if (transport_ == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    std::size_t sent = 0;
    for (const auto &frame : frames.value()) {
        if (auto outcome = send_frame(frame); !outcome) {
            return outcome.error();
        }
        ++sent;
    }
    return sent;
}

// ---------------------------------------------------------------------------
// Media pump.
// ---------------------------------------------------------------------------

core::Result<bool> WiredSession::poll_transport() {
    return poll_transport_impl(std::nullopt);
}

core::Result<bool> WiredSession::poll_transport(core::Deadline::Clock::time_point until) {
    return poll_transport_impl(until);
}

core::Result<bool> WiredSession::poll_transport_impl(
    std::optional<core::Deadline::Clock::time_point> until) {
    last_reject_.reset();
    last_queue_rejection_.reset();
    if (transport_ == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    const auto deadline = io_deadline();
    if (auto checked = deadline.check(); !checked) {
        return io_error(checked.error(), deadline);
    }
    core::DeadlineCancellation bounded{deadline};
    transport_->set_io_deadline(deadline.until);
    if (auto opened = transport_->open(bounded.token()); !opened) {
        return io_error(opened.error(), deadline);
    }
    for (const auto& [role, sink] : av_sinks_) { (void)role; sink->observe_receive(true); }
    auto frame = [&]() -> core::Result<std::optional<std::vector<std::byte>>> {
        if (until) { return transport_->poll_receive(std::min(*until, deadline.until)); }
        auto received = transport_->receive();
        if (!received) { return received.error(); }
        return std::optional{std::move(received).value()};
    }();
    for (const auto& [role, sink] : av_sinks_) { (void)role; sink->observe_receive(false); }
    if (!frame) {
        return io_error(frame.error(), deadline);
    }
    if (auto checked = deadline.check(); !checked) {
        return io_error(checked.error(), deadline);
    }
    if (!frame.value()) { return false; }
    auto chunks = parser_.feed(*frame.value());
    if (!chunks) {
        return chunks.error();
    }
    bool routed = false;
    for (const auto &chunk : chunks.value()) {
        auto message = reassembler_.push(chunk);
        if (!message) {
            return message.error();
        }
        if (message.value().has_value()) {
            if (auto handled = route(*message.value()); !handled) {
                return handled.error();
            }
            routed = true;
            if (token().stop_requested()) { break; }
        }
    }
    return routed;
}

core::Result<void> WiredSession::route(const transport::Message &message) {
    if (message.channel == 0) {
        return handle_control(message.payload);
    }
    if (!authenticated()) {
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    if (!negotiator_.has_value()) {
        const auto id = message.payload.size() >= 2
            ? std::optional<std::uint16_t>{static_cast<std::uint16_t>(
                (static_cast<std::uint16_t>(message.payload[0]) << 8U) |
                static_cast<std::uint16_t>(message.payload[1]))} : std::nullopt;
        last_reject_ = RejectedFrame{message.channel, id};
        return not_configured();
    }
    const auto id = message.payload.size() >= 2
        ? std::optional<std::uint16_t>{static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(message.payload[0]) << 8U) |
            static_cast<std::uint16_t>(message.payload[1]))} : std::nullopt;
    const protocol::ServiceKey service{message.channel};
    // Like pinned SensorSourceService::messageHandler, service channels carry
    // their own open requests before ordinary service-message dispatch.
    if (id == static_cast<std::uint16_t>(protocol::ControlMessageId::channel_open_request)) {
        auto request = protocol::decode_channel_open_request(message.payload);
        if (!request) {
            request = protocol::decode_in_band_channel_open_request(message.payload);
        }
        protocol::ChannelOpenResponse response{protocol::ChannelOpenStatus::unsupported};
        if (!request || request.value().service == service) {
            const auto opened = negotiator_->open({request ? request.value().priority : 0, service});
            if (opened) { response = opened.value(); }
        }
        auto payload = protocol::encode(response);
        if (!payload) { return payload.error(); }
        if (auto sent = send_service_payload(message.channel, std::move(payload).value(),
                                              transport::MessageKind::control); !sent) {
            return sent.error();
        }
        if (response.status == protocol::ChannelOpenStatus::success) { ++channel_opens_; }
        return {};
    }
    if (id == static_cast<std::uint16_t>(protocol::ControlMessageId::channel_close_notification)) {
        for (const auto &descriptor : descriptors_) {
            if (descriptor.id != service) { continue; }
            const auto sink = av_sinks_.find(descriptor.role);
            if (sink != av_sinks_.end()) { sink->second->reset(); }
        }
        ++channel_closes_;
        return {};
    }
    if (!negotiator_->is_open(service)) {
        const auto opened = negotiator_->open({0, service});
        if (opened && opened.value().status == protocol::ChannelOpenStatus::success) {
            ++channel_opens_;
        }
    }
    ++sequence_;
    const auto at = deps_.clock.now();
    auto dispatched = negotiator_->dispatch(
        protocol::WireMessageView{message.channel, sequence_, at, message.payload});
    if (!dispatched && dispatched.error().code() == ErrorCode::channel_not_registered) {
        last_reject_ = RejectedFrame{message.channel, id};
    }
    if (!dispatched && dispatched.error().code() == ErrorCode::ipc_queue_full) {
        for (const auto &descriptor : descriptors_) {
            if (descriptor.id != service) { continue; }
            const auto sink = av_sinks_.find(descriptor.role);
            if (sink != av_sinks_.end()) {
                last_queue_rejection_ = sink->second->last_queue_rejection();
            }
            break;
        }
    }
    return dispatched;
}

core::Result<void> WiredSession::handle_control(std::span<const std::byte> payload) {
    if (payload.size() < 2) {
        return bad_argument();
    }
    const auto high = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0]));
    const auto low = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[1]));
    const auto id = static_cast<protocol::ControlMessageId>((high << 8U) | low);
    switch (id) {
    case protocol::ControlMessageId::version_response: {
        if (auth_phase_ != AuthPhase::awaiting_version) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        auto response = protocol::decode_version_response(payload);
        if (!response) {
            return response.error();
        }
        if (response.value().status != 0) {
            (void)fail(Error{ErrorCode::protocol_version_mismatch});
            return Error{ErrorCode::protocol_version_mismatch};
        }
        ++version_responses_;
        auth_phase_ = AuthPhase::ssl_handshake;
        return drive_ssl_handshake();
    }
    case protocol::ControlMessageId::encapsulated_ssl: {
        // P2 phase guard: handshake chunks belong to the ssl_handshake phase
        // only. Before version_response this path used to reach drive_ssl_handshake
        // and could authenticate, bypassing version negotiation entirely.
        // After authentication re-driven chunks are accepted and ignored (they
        // must never re-send auth_complete).
        if (auth_phase_ == AuthPhase::awaiting_version) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        if (auth_phase_ == AuthPhase::authenticated) {
            return {};
        }
        auto chunk = protocol::decode_encapsulated_ssl(payload);
        if (!chunk) {
            return chunk.error();
        }
        if (!cipher_) {
            return not_configured();
        }
        ++encapsulated_ssls_;
        cipher_->give_handshake(chunk.value().bytes);
        return drive_ssl_handshake();
    }
    case protocol::ControlMessageId::auth_complete: {
        // AUTH_COMPLETE is HU->Phone, never an inbound authentication signal.
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    case protocol::ControlMessageId::service_discovery_request: {
        if (!authenticated()) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        if (!negotiator_.has_value()) {
            return not_configured();
        }
        if (auto sent = announce_services(); !sent) {
            return sent.error();
        }
        return {};
    }
    case protocol::ControlMessageId::channel_open_request: {
        if (!authenticated()) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        if (!negotiator_.has_value()) {
            return not_configured();
        }
        auto request = protocol::decode_channel_open_request(payload);
        if (!request) {
            return request.error();
        }
        auto opened = negotiator_->open(request.value());
        if (!opened) {
            return opened.error();
        }
        if (auto sent = send_payload(opened.value()); !sent) {
            return sent.error();
        }
        ++channel_opens_;
        return {};
    }
    case protocol::ControlMessageId::audio_focus_request: {
        if (!authenticated()) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        const auto request = protocol::decode_audio_focus_request(payload);
        if (!request) { return request.error(); }
        const auto state = request.value().type == protocol::AudioFocusRequestType::release
            ? protocol::AudioFocusState::loss : protocol::AudioFocusState::gain;
        auto encoded = protocol::encode(protocol::AudioFocusNotification{state, false});
        if (!encoded) { return encoded.error(); }
        if (auto sent = send_frames(std::move(encoded).value(), true); !sent) {
            return sent.error();
        }
        return {};
    }
    case protocol::ControlMessageId::nav_focus_request: {
        if (!authenticated()) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        const auto request = protocol::decode_nav_focus_request(payload);
        if (!request) { return request.error(); }
        // An absent optional selection defaults to projected; native releases focus.
        auto encoded = protocol::encode(
            protocol::NavFocusNotification{request.value().type.value_or(protocol::NavFocusType::projected)});
        if (!encoded) { return encoded.error(); }
        if (auto sent = send_frames(std::move(encoded).value(), true); !sent) {
            return sent.error();
        }
        return {};
    }
    case protocol::ControlMessageId::byebye_request: {
        if (!authenticated()) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        const auto request = protocol::decode_byebye_request(payload);
        if (!request) { return request.error(); }
        auto encoded = protocol::encode(protocol::ByeByeResponse{});
        if (!encoded) { return encoded.error(); }
        if (auto sent = send_frames(std::move(encoded).value(), true); !sent) {
            (void)fail(sent.error());
            return sent.error();
        }
        request_stop();
        tick();
        return {};
    }
    case protocol::ControlMessageId::ping_request: {
        auto request = protocol::decode_ping_request(payload);
        if (!request) {
            return request.error();
        }
        protocol::PingResponse reply{request.value().timestamp, request.value().data};
        auto encoded = protocol::encode(reply);
        if (!encoded) {
            return encoded.error();
        }
        if (auto sent = send_frames(std::move(encoded).value(), false); !sent) {
            return sent.error();
        }
        ++pings_answered_;
        return {};
    }
    default:
        // Pinned ControlServiceChannel.cpp:231-234 continues receiving unknown IDs.
        ++unhandled_controls_;
        if (unhandled_controls_ <= 8) {
            diagnostics::Event event{};
            event.session = session_id_;
            event.kind = diagnostics::EventKind::transport_stats;
            event.monotonic_time = deps_.clock.now();
            event.fields.push_back(diagnostics::EventField::count("msg_id", (high << 8U) | low));
            event.fields.push_back(diagnostics::EventField::count(
                "auth_phase", static_cast<std::int64_t>(auth_phase_)));
            event.fields.push_back(diagnostics::EventField::count(
                "payload_bytes", static_cast<std::int64_t>(payload.size())));
            deps_.diagnostics.emit(event);
            ++diagnostics_emitted_;
        }
        return {};
    }
}

core::Result<void> WiredSession::drive_ssl_handshake() {
    if (auth_phase_ == AuthPhase::authenticated) {
        return {};
    }
    if (!cipher_) {
        return Error{ErrorCode::channel_not_registered};
    }
    auto stepped = cipher_->step_handshake();
    if (!stepped) {
        (void)fail(Error{ErrorCode::transport_unauthorized_peer});
        return stepped.error();
    }
    auto out = cipher_->take_handshake();
    if (!out.empty()) {
        if (auto sent = send_encapsulated_ssl(std::move(out)); !sent) {
            return sent.error();
        }
    }
    if (cipher_->is_active()) {
        ssl_active_ = true;
        // AuthResponse.status is int32; pinned MessageStatus::STATUS_SUCCESS = 0.
        auto payload = protocol::encode(protocol::AuthComplete{0});
        if (!payload) {
            return payload.error();
        }
        if (auto sent = send_frames(std::move(payload).value(), false); !sent) {
            return sent.error();
        }
        ++auth_completes_;
        auth_phase_ = AuthPhase::authenticated;
    }
    return {};
}

core::Result<std::size_t> WiredSession::send_encapsulated_ssl(std::vector<std::byte> bytes) {
    auto payload = protocol::encode(protocol::EncapsulatedSsl{std::move(bytes)});
    if (!payload) {
        return payload.error();
    }
    return send_frames(std::move(payload).value(), false);
}

core::Result<std::size_t>
WiredSession::send_payload(const protocol::ServiceDiscoveryResponse &response) {
    auto payload = protocol::encode(response);
    if (!payload) {
        return payload.error();
    }
    return send_frames(std::move(payload).value(), true);
}

core::Result<std::size_t>
WiredSession::send_payload(const protocol::ChannelOpenResponse &response) {
    auto payload = protocol::encode(response);
    if (!payload) {
        return payload.error();
    }
    return send_frames(std::move(payload).value(), true);
}

core::Result<std::size_t> WiredSession::send_frames(std::vector<std::byte> payload,
                                                    bool encrypted) {
    if (transport_ == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    // AASDK parity (finding P1-A): every control-channel message goes out as
    // MessageType::SPECIFIC (pinned ControlServiceChannel.cpp sends SPECIFIC at
    // all ten send sites; MessageType.hpp: SPECIFIC = 0, CONTROL = 1 << 2), so
    // the frame kind bit stays clear (conformance D19: 0x03 draws responses
    // from the S24 phone framer while 0x07 is ignored).
    transport::Message message{std::uint8_t{0}, transport::MessageKind::specific,
                               std::move(payload)};
    transport::Encryption encryption = transport::Encryption::plain;
    transport::PayloadCipher *cipher = nullptr;
    if (encrypted) {
        if (!cipher_ || !cipher_->is_active()) {
            return Error{ErrorCode::transport_unauthorized_peer};
        }
        encryption = transport::Encryption::encrypted;
        cipher = cipher_.get();
    }
    auto frames = transport::encode_message(message, encryption, cipher);
    if (!frames) {
        return frames.error();
    }
    std::size_t sent = 0;
    for (const auto &frame : frames.value()) {
        if (auto outcome = send_frame(frame); !outcome) {
            return outcome.error();
        }
        ++sent;
    }
    return sent;
}

// ---------------------------------------------------------------------------
// Input (task 26).
// ---------------------------------------------------------------------------

core::Result<void> WiredSession::send_service_payload(std::uint8_t channel,
                                                       std::vector<std::byte> payload,
                                                       transport::MessageKind kind) {
    if (!authenticated()) { return Error{ErrorCode::transport_unauthorized_peer}; }
    // Service open replies use CONTROL; ordinary service payloads stay SPECIFIC.
    // D19's SPECIFIC evidence applies to control channel 0, not service opens.
    transport::Message message{channel, kind, std::move(payload)};
    auto frames = transport::encode_message(message, transport::Encryption::encrypted, cipher_.get());
    if (!frames) { return frames.error(); }
    for (const auto& frame : frames.value()) {
        if (auto sent = send_frame(frame); !sent) { return sent.error(); }
    }
    return {};
}

core::Result<void> WiredSession::send_channel_payload(protocol::ChannelRole role,
                                                       std::vector<std::byte> payload) {
    if (!authenticated()) { return Error{ErrorCode::transport_unauthorized_peer}; }
    for (const auto& service : descriptors_) {
        if (service.role != role) { continue; }
        return send_service_payload(static_cast<std::uint8_t>(service.id.value), std::move(payload));
    }
    return Error{ErrorCode::channel_not_registered};
}

core::Result<void> WiredSession::submit_input(const ipc::InputEvent &event) {
    const auto video = av_sinks_.find(protocol::ChannelRole::video);
    if (video != av_sinks_.end() && !video->second->input_focus_allowed()) {
        return Error{ErrorCode::channel_unsupported_capability};
    }
    if (!input_service_.has_value()) {
        return Error{ErrorCode::channel_unsupported_capability};
    }
    return input_service_->submit(event);
}

std::optional<std::uint8_t> WiredSession::input_channel() const noexcept {
    for (const auto &service : descriptors_) {
        if (service.role != protocol::ChannelRole::input) {
            continue;
        }
        const auto value = service.id.value;
        if (value > 0 && value <= 0xFF) {
            return static_cast<std::uint8_t>(value);
        }
    }
    return std::nullopt;
}

core::Result<void> WiredSession::forward_input(const input::NormalizedInput &event) {
    const auto channel = input_channel();
    if (!channel.has_value() || transport_ == nullptr || !authenticated()) {
        ++input_send_failures_;
        return Error{ErrorCode::transport_unauthorized_peer};
    }
    // Finding F2: the phone-facing payload is the AAP InputReport protobuf
    // (message id 0x8001 + body), converted at this boundary - not the local
    // 24-byte IPC record. The normalized event is capability-gated upstream.
    auto payload =
        protocol::encode_input_report(event, static_cast<std::uint64_t>(deps_.clock.now().count));
    if (!payload) {
        ++input_send_failures_;
        return payload.error();
    }
    transport::Message message{*channel, transport::MessageKind::specific,
                               std::move(payload).value()};
    auto frames =
        transport::encode_message(message, transport::Encryption::encrypted, cipher_.get());
    if (!frames) {
        ++input_send_failures_;
        return frames.error();
    }
    for (const auto &frame : frames.value()) {
        if (auto outcome = send_frame(frame); !outcome) {
            ++input_send_failures_;
            return outcome.error();
        }
    }
    ++input_sends_;
    return {};
}

// ---------------------------------------------------------------------------
// Diagnostics (task 19): redacted lifecycle telemetry.
// ---------------------------------------------------------------------------

void WiredSession::emit(std::string_view identity_key) {
    diagnostics::Event event{};
    event.session = session_id_;
    event.kind = diagnostics::EventKind::session_state_changed;
    event.monotonic_time = deps_.clock.now();
    event.fields.push_back(diagnostics::EventField::text(
        "to_state", std::string{session::to_string(lifecycle_.state())}));
    event.fields.push_back(diagnostics::EventField::text("transport", "usb"));
    if (lifecycle_.state() == session::State::failed) {
        if (const auto failure = lifecycle_.failure()) {
            event.fields.push_back(diagnostics::EventField::count(
                "error_code", static_cast<std::int64_t>(failure->code())));
        }
    }
    if (!identity_key.empty()) {
        // Pseudonymized by the task-19 boundary: the raw key never leaves.
        event.fields.push_back(
            diagnostics::EventField::identifier("phone_id", std::string{identity_key}));
    }
    deps_.diagnostics.emit(event);
    ++diagnostics_emitted_;
}

void WiredSession::note_state(std::string_view identity_key) {
    if (lifecycle_.state() == last_state_) {
        return;
    }
    last_state_ = lifecycle_.state();
    switch (last_state_) {
    case session::State::reconnecting:
    case session::State::failed:
    case session::State::stopping:
    case session::State::disconnected: reset_owned_av(); break;
    default: break;
    }
    emit(identity_key);
    // A state change is the only place a recovery attempt can RESOLVE outside
    // its opening event: acceptance is gated on State::active (item 1), and any
    // terminal exit ends the attempt as failed so it can never be accepted
    // later (deadline/negotiation failure, or a cancelled teardown).
    switch (last_state_) {
    case session::State::active:
        resolve_recovery(RecoveryStatus::accepted, std::nullopt);
        break;
    case session::State::failed: {
        std::optional<std::int64_t> code;
        if (lifecycle_.failure().has_value()) {
            code = static_cast<std::int64_t>(lifecycle_.failure()->code());
        }
        resolve_recovery(RecoveryStatus::failed, code);
        break;
    }
    case session::State::stopping:
    case session::State::disconnected:
        resolve_recovery(RecoveryStatus::failed,
                         static_cast<std::int64_t>(ErrorCode::cancelled));
        break;
    default:
        break;
    }
}

void WiredSession::note_recovery(session::Event event, const core::Result<void> &outcome) {
    switch (event) {
    case session::Event::link_lost:
        if (outcome.has_value()) {
            recovery_in_flight_ = true;
            recovery_reason_ = "link lost";
            emit_recovery("link lost", RecoveryStatus::pending, std::nullopt);
        }
        break;
    case session::Event::reconnect:
    case session::Event::recover: {
        ++reconnect_attempts_;
        const char *reason = event == session::Event::recover ? "peer disconnected" : "link lost";
        recovery_reason_ = reason;
        recovery_in_flight_ = true;
        if (!outcome.has_value()) {
            resolve_recovery(RecoveryStatus::failed,
                             std::optional<std::int64_t>{
                                 static_cast<std::int64_t>(outcome.error().code())});
            break;
        }
        // Gate acceptance on active (item 1): a successful reconnect only
        // CONNECTS, so the attempt stays pending until the state reaches
        // active; only recover can land straight in active.
        if (lifecycle_.state() == session::State::active) {
            resolve_recovery(RecoveryStatus::accepted, std::nullopt);
        } else {
            emit_recovery(reason, RecoveryStatus::pending, std::nullopt);
        }
        break;
    }
    default:
        break;
    }
}

void WiredSession::resolve_recovery(RecoveryStatus status, std::optional<std::int64_t> code) {
    if (!recovery_in_flight_) {
        return;
    }
    recovery_in_flight_ = false;
    if (status == RecoveryStatus::failed) {
        ++recovery_failures_;
    }
    emit_recovery(recovery_reason_, status, code);
}

void WiredSession::emit_recovery(const char *reason, RecoveryStatus status,
                                 std::optional<std::int64_t> code) {
    const std::string_view status_text = status == RecoveryStatus::accepted ? "accepted"
                                         : status == RecoveryStatus::failed ? "failed"
                                                                            : "pending";
    diagnostics::Event event{};
    event.session = session_id_;
    event.kind = diagnostics::EventKind::reconnect;
    event.monotonic_time = deps_.clock.now();
    event.fields.push_back(diagnostics::EventField::text("reason", std::string{reason}));
    event.fields.push_back(diagnostics::EventField::text("status", std::string{status_text}));
    event.fields.push_back(
        diagnostics::EventField::count("retries", static_cast<std::int64_t>(reconnect_attempts_)));
    event.fields.push_back(
        diagnostics::EventField::count("errors", static_cast<std::int64_t>(recovery_failures_)));
    if (code.has_value()) {
        event.fields.push_back(diagnostics::EventField::count("code", *code));
    }
    event.fields.push_back(diagnostics::EventField::text("transport", "usb"));
    // Identity attribution (item 3, same attribution as the link-error event):
    // the recovery record names the admitted phone so a harness can correlate a
    // state/flow transition to the intended device. Pseudonymized by the
    // boundary like every identity that leaves.
    if (!last_identity_key_.empty()) {
        event.fields.push_back(
            diagnostics::EventField::identifier("phone_id", last_identity_key_));
    }
    deps_.diagnostics.emit(event);
    ++diagnostics_emitted_;
}

} // namespace aa::app
