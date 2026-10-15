// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WiredSession.hpp>
#include <aa/core/Deadline.hpp>

#include <aa/protocol/InputReport.hpp>

#include <string>
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

WiredSession::~WiredSession() = default;

// ---------------------------------------------------------------------------
// Session / trust delegation. No approval happens here: the store decides.
// ---------------------------------------------------------------------------

core::Result<void> WiredSession::apply(session::Event event) {
    auto outcome = lifecycle_.apply(event);
    note_state();
    return outcome;
}

core::Result<void> WiredSession::phone_discovered(const trust::PhoneIdentity &identity) {
    auto outcome = lifecycle_.phone_discovered(identity);
    note_state(identity.key);
    return outcome;
}

core::Result<void> WiredSession::fail(Error reason) {
    auto outcome = lifecycle_.fail(reason);
    note_state();
    return outcome;
}

void WiredSession::tick() { lifecycle_.tick(); }

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
    version_responses_ = 0;
    pings_answered_ = 0;
    encapsulated_ssls_ = 0;
    auth_completes_ = 0;
}

void WiredSession::attach_cipher(std::unique_ptr<transport::HandshakingCipher> cipher) noexcept {
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

core::Result<std::size_t> WiredSession::announce_services() {
    if (!negotiator_.has_value()) {
        return not_configured();
    }
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
    auto frame = transport_->receive();
    if (!frame) {
        return io_error(frame.error(), deadline);
    }
    auto chunks = parser_.feed(frame.value());
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
        return not_configured();
    }
    ++sequence_;
    const auto at = deps_.clock.now();
    return negotiator_->dispatch(
        protocol::WireMessageView{message.channel, sequence_, at, message.payload});
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
        if (auto sent = send_payload(negotiator_->start()); !sent) {
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
        return bad_argument();
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
    std::fprintf(stderr, "HS-DBG step has_value=%d val=%d active=%d\n",
                 (int)stepped.has_value(),
                 (int)(stepped.has_value() ? stepped.value() : false),
                 (int)cipher_->is_active());
    if (!stepped) {
        (void)fail(Error{ErrorCode::transport_unauthorized_peer});
        return stepped.error();
    }
    auto out = cipher_->take_handshake();
    std::fprintf(stderr, "HS-DBG out=%zu\n", out.size());
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

core::Result<void> WiredSession::submit_input(const ipc::InputEvent &event) {
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
    emit(identity_key);
}

} // namespace aa::app
