// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/app/WiredSession.hpp>

#include <array>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

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
    void on_normalized(const input::NormalizedInput &event) override {
        owner_.forward_input(event);
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

void WiredSession::attach_transport(transport::Transport *transport) noexcept {
    transport_ = transport;
    lifecycle_.attach_transport(transport);
    // A fresh transport is a fresh link (the lifecycle never reuses a retired
    // handle): drop every byte of the previous wire stream and all negotiated
    // channel-open state, so a reconnect cannot resurrect the old session from
    // cached framing/routing state (channels::Negotiator::reset() contract).
    parser_ = transport::FrameStreamParser{};
    reassembler_ = transport::MessageReassembler{};
    if (negotiator_.has_value()) {
        negotiator_->reset();
    }
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
        if (auto outcome = transport_->send(frame); !outcome) {
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
    auto frame = transport_->receive();
    if (!frame) {
        return frame.error();
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
    if (!negotiator_.has_value()) {
        return not_configured();
    }
    ++sequence_;
    const auto at = deps_.clock.now();
    return negotiator_->dispatch(
        protocol::WireMessageView{message.channel, sequence_, at, message.payload});
}

core::Result<void> WiredSession::handle_control(std::span<const std::byte> payload) {
    if (!negotiator_.has_value()) {
        return not_configured();
    }
    if (payload.size() < 2) {
        return bad_argument();
    }
    const auto high = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[0]));
    const auto low = static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[1]));
    const auto id = static_cast<protocol::ControlMessageId>((high << 8U) | low);
    switch (id) {
    case protocol::ControlMessageId::service_discovery_request: {
        if (auto sent = send_payload(negotiator_->start()); !sent) {
            return sent.error();
        }
        return {};
    }
    case protocol::ControlMessageId::channel_open_request: {
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
    case protocol::ControlMessageId::version_response:
        ++version_responses_;
        return {};
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
        if (auto sent = send_frames(std::move(encoded).value()); !sent) {
            return sent.error();
        }
        ++pings_answered_;
        return {};
    }
    default:
        return bad_argument();
    }
}

core::Result<std::size_t>
WiredSession::send_payload(const protocol::ServiceDiscoveryResponse &response) {
    auto payload = protocol::encode(response);
    if (!payload) {
        return payload.error();
    }
    return send_frames(std::move(payload).value());
}

core::Result<std::size_t>
WiredSession::send_payload(const protocol::ChannelOpenResponse &response) {
    auto payload = protocol::encode(response);
    if (!payload) {
        return payload.error();
    }
    return send_frames(std::move(payload).value());
}

core::Result<std::size_t> WiredSession::send_frames(std::vector<std::byte> payload) {
    if (transport_ == nullptr) {
        return Error{ErrorCode::transport_unavailable};
    }
    transport::Message message{std::uint8_t{0}, transport::MessageKind::control,
                               std::move(payload)};
    auto frames = transport::encode_message(message, transport::Encryption::plain, nullptr);
    if (!frames) {
        return frames.error();
    }
    std::size_t sent = 0;
    for (const auto &frame : frames.value()) {
        if (auto outcome = transport_->send(frame); !outcome) {
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

void WiredSession::forward_input(const input::NormalizedInput &event) {
    const auto channel = input_channel();
    if (!channel.has_value() || transport_ == nullptr) {
        ++input_send_failures_;
        return;
    }
    // Reuse the project's bounded input record codec for the forwarded payload
    // (24-byte AAP input channel record shape); the AAP-protobuf mapping is the
    // AASDK adapter's concern, not reimplemented here.
    const ipc::InputEvent record = std::visit(
        [](const auto &normalized) -> ipc::InputEvent {
            using T = std::decay_t<decltype(normalized)>;
            if constexpr (std::is_same_v<T, input::NormalizedKey>) {
                return ipc::KeyEvent{static_cast<std::int32_t>(normalized.keycode),
                                     normalized.down};
            } else {
                return ipc::TouchEvent{normalized.x, normalized.y, normalized.action};
            }
        },
        event);
    std::array<std::uint8_t, ipc::kInputRecordBytes> bytes{};
    ipc::encode_input_record(bytes, record);

    std::vector<std::byte> payload(ipc::kInputRecordBytes);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        payload[index] = static_cast<std::byte>(bytes[index]);
    }
    transport::Message message{*channel, transport::MessageKind::specific, std::move(payload)};
    auto frames = transport::encode_message(message, transport::Encryption::plain, nullptr);
    if (!frames) {
        ++input_send_failures_;
        return;
    }
    for (const auto &frame : frames.value()) {
        if (auto outcome = transport_->send(frame); !outcome) {
            ++input_send_failures_;
            return;
        }
    }
    ++input_sends_;
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
