// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Wired end-to-end projection integrator (task 30). This is the WIRING layer
// that composes the already-qualified modules into one wired projection path;
// it reimplements none of them:
//
//   transport::Transport (task 28 USB/AOA, or a fake)  ->  session::Lifecycle
//   (task 17: admission, deadlines, cancel, reconnect)  ->  the shared frame
//   codec (task 16 FrameStreamParser/MessageReassembler)  ->  channels::
//   Negotiator/ServiceRegistry (task 18: discovery/open/dispatch)  ->  the
//   registered per-role protocol::MessageSink consumers (the task-22 video
//   producer path, the task-25 audio router, ...). Input (task 26) runs the
//   receiver-side input::InputService over an ipc::InputEvent and forwards the
//   normalized event onto the input service channel over the same transport.
//   diagnostics::Diagnostics (task 19) records redacted lifecycle/transport
//   telemetry along the path.
//
// TRUST (task 27): admission is delegated to session::Lifecycle, which consults
// the trust::TrustStore on every connect/reconnect/recover. This class never
// approves a phone: an approved phone reaches active, an unknown/rejected one
// fails closed, and a mid-session revoke (store no longer approves) makes the
// next reconnect attempt fail closed instead of resuming.
//
// Threading: caller-driven; one thread at a time (the session thread). No
// internal threads are created. The attached transport is owned elsewhere and
// must outlive the session while attached.
//
// AAP wire encoding note: control/service frames use the project's own message
// codec (protocol::encode + transport::encode_message). The phone-bound input
// payload is the real AAP InputReport protobuf (protocol::encode_input_report),
// converted at the input boundary - not the local IPC record.

#include <aa/channels/AvSink.hpp>
#include <aa/channels/Negotiator.hpp>
#include <aa/channels/Services.hpp>
#include <aa/core/Cancellation.hpp>
#include <aa/core/Error.hpp>
#include <aa/core/Ids.hpp>
#include <aa/core/Result.hpp>
#include <aa/diagnostics/Diagnostics.hpp>
#include <aa/input/Capability.hpp>
#include <aa/input/InputService.hpp>
#include <aa/ipc/InputSocket.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/protocol/Protocol.hpp>
#include <aa/session/Clock.hpp>
#include <aa/session/Events.hpp>
#include <aa/session/Lifecycle.hpp>
#include <aa/session/Session.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/Framing.hpp>
#include <aa/transport/Transport.hpp>
#include <aa/trust/Trust.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace aa::app {

// Non-owning collaborators supplied by the receiver entry point (or a test).
struct WiredSessionDeps final {
    trust::TrustStore &trust;              // task 27 admission source
    session::MonotonicClock &clock;        // session-phase clock
    diagnostics::Diagnostics &diagnostics; // task 19 redacted telemetry
};

struct WiredSessionConfig final {
    // Caller phase budgets plus the wired reconnect budget selection.
    session::LifecycleConfig lifecycle{{{}, {}, core::Milliseconds{15'000}, {}}};
    // task 26 capability-gated input (bench defaults: keyboard/D-pad +
    // mouse-to-touch, no vehicle sensors).
    bool input_enabled{true};
    input::CapabilityDescriptor input = input::CapabilityDescriptor::bench_defaults();
    core::SessionId session{core::SessionId{1}};
};

class WiredSession final {
  public:
    struct RejectedFrame final {
        std::uint8_t channel{};
        std::optional<std::uint16_t> msg_id{};
        [[nodiscard]] friend bool operator==(const RejectedFrame&,
                                             const RejectedFrame&) = default;
    };

    WiredSession(WiredSessionDeps deps, WiredSessionConfig config = WiredSessionConfig{});
    ~WiredSession();

    WiredSession(const WiredSession &) = delete;
    WiredSession &operator=(const WiredSession &) = delete;
    WiredSession(WiredSession &&) = delete;
    WiredSession &operator=(WiredSession &&) = delete;

    // ---- Session / trust (thin delegation to session::Lifecycle) ----
    // Every call re-decides admission from the TrustStore; nothing is approved
    // here. phone_discovered is the wired identity event; a revoke that removes
    // the identity makes the following reconnect/recover fail closed.
    [[nodiscard]] core::Result<void> apply(session::Event event);
    [[nodiscard]] core::Result<void> phone_discovered(const trust::PhoneIdentity &identity);
    [[nodiscard]] core::Result<void> fail(Error reason);
    void tick();
    void request_stop() noexcept;

    // Per-link lifecycle: attach_transport() begins a FRESH link and drops
    // every per-link artifact of the previous one - wire parser, reassembler
    // and its cipher binding, negotiated channel state, the auth phase and the
    // previous link's cipher. Call order per link is attach_transport() first,
    // then attach_cipher(); sealing is impossible until the new link's cipher
    // handshake completes.
    void attach_transport(transport::Transport *transport) noexcept;

    // AAP authentication/TLS handshake (finding F1). Attach the negotiated TLS
    // cipher seam (the HU-side handshake cipher) AFTER attach_transport(); the
    // same cipher object is bound into the message reassembler so inbound E=1
    // records open with it once the handshake completes (P1-C). Attach a FRESH
    // cipher per link (initial attach and every reconnect), bound to the
    // currently admitted identity - never reuse a prior link's cipher.
    // The phone-side control progression is: version_response -> exchange
    // encapsulated_ssl handshake chunks -> auth_complete. Media/input channel
    // traffic is refused until authenticated(); post-auth outbound frames use
    // the negotiated cipher (not plaintext).
    void attach_cipher(std::unique_ptr<transport::HandshakingCipher> cipher) noexcept;
    [[nodiscard]] bool authenticated() const noexcept {
        return auth_phase_ == AuthPhase::authenticated;
    }

    [[nodiscard]] session::State state() const noexcept { return lifecycle_.state(); }
    [[nodiscard]] std::optional<Error> failure() const noexcept { return lifecycle_.failure(); }
    [[nodiscard]] core::CancellationToken token() const noexcept { return lifecycle_.token(); }
    [[nodiscard]] core::Deadline io_deadline(bool writing = false) const noexcept;

    // ---- Channel services (task 18) ----
    // Validate the negotiated service descriptors and build the negotiator.
    // Call once before announce_services/register_sink-over-registry.
    [[nodiscard]] core::Result<void> configure(std::vector<protocol::ServiceDescriptor> services);

    // Register the consumer for one channel role (e.g. the video producer
    // forwarder, the audio router forwarder). Non-owning; must outlive the
    // session. Registering a role twice is refused.
    [[nodiscard]] core::Result<void> register_sink(protocol::ChannelRole role,
                                                    protocol::MessageSink &sink);

    // Own the negotiated AV boundary; raw_sink remains non-owning and receives
    // only stripped media. Requires a configured AV descriptor for this role.
    [[nodiscard]] core::Result<void> register_av_sink(protocol::ChannelRole role,
                                                     protocol::MessageSink &raw_sink);

    // Publish the service-discovery response for the configured services and
    // send it over the transport. Returns the number of frames sent.
    [[nodiscard]] core::Result<std::size_t> announce_services();

    // AAP VersionRequest (HU -> phone) as the pinned AASDK wire form: control
    // channel, SPECIFIC frame kind (conformance F-A; the CONTROL-kind variant
    // is ignored by real phones). This is the first frame the phone expects
    // after the wired link is up. Returns the number of frames sent.
    [[nodiscard]] core::Result<std::size_t> send_version_request(std::uint16_t major,
                                                                 std::uint16_t minor);

    // ---- Media pump ----
    // Read exactly one frame from the attached transport, reassemble it and
    // route it (control -> negotiation reply, media -> the registered sink).
    // Returns true when a complete message was routed. A transport/reassembly
    // failure is returned typed (and the underlying channel is terminal).
    [[nodiscard]] core::Result<bool> poll_transport();

    // ---- Input (task 26) ----
    // Capability-gate one display input event and forward the normalized event
    // onto the input service channel over the transport.
    [[nodiscard]] core::Result<void> submit_input(const ipc::InputEvent &event);

    [[nodiscard]] core::Result<void>
    send_channel_payload(protocol::ChannelRole role, std::vector<std::byte> payload);

    // Observed counters (diagnostics/tests). The negotiation counters
    // (channel_opens, version_responses, pings_answered, encapsulated_ssls,
    // auth_completes, unhandled_controls) are PER-LINK: attach_transport() zeroes them with the
    // link state. input_sends/input_send_failures/diagnostics_emitted are
    // run-cumulative telemetry.
    [[nodiscard]] std::uint64_t diagnostics_emitted() const noexcept {
        return diagnostics_emitted_;
    }

    [[nodiscard]] std::uint64_t input_sends() const noexcept { return input_sends_; }
    [[nodiscard]] std::uint64_t input_send_failures() const noexcept {
        return input_send_failures_;
    }
    [[nodiscard]] std::uint64_t channel_opens() const noexcept { return channel_opens_; }
    [[nodiscard]] std::uint64_t unhandled_controls() const noexcept { return unhandled_controls_; }
    [[nodiscard]] std::uint64_t channel_closes() const noexcept { return channel_closes_; }
    [[nodiscard]] std::optional<RejectedFrame> last_reject() const noexcept { return last_reject_; }
    [[nodiscard]] std::uint64_t version_responses() const noexcept { return version_responses_; }
    [[nodiscard]] std::uint64_t pings_answered() const noexcept { return pings_answered_; }
    [[nodiscard]] std::uint64_t encapsulated_ssls() const noexcept { return encapsulated_ssls_; }
    [[nodiscard]] std::uint64_t auth_completes() const noexcept { return auth_completes_; }

    // ---- Recovery telemetry (task 31 additive seam) ----
    // Counts reconnect/recover attempts and failed recovery outcomes observed on
    // the session; each is emitted as a redacted EventKind::reconnect diagnostic.
    // Purely additive observability: the lifecycle transitions and the delegated
    // TrustStore admission decisions are unchanged.
    [[nodiscard]] std::uint64_t reconnect_attempts() const noexcept { return reconnect_attempts_; }
    [[nodiscard]] std::uint64_t recovery_failures() const noexcept { return recovery_failures_; }

  private:
    class InputForwarder;
    [[nodiscard]] core::Result<void> send_frame(std::span<const std::byte> frame);
    [[nodiscard]] Error io_error(Error reason, const core::Deadline &deadline);

    // AAP authentication progression (finding F1).
    enum class AuthPhase { awaiting_version, ssl_handshake, authenticated };

    [[nodiscard]] core::Result<void> route(const transport::Message &message);
    [[nodiscard]] core::Result<void> handle_control(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> drive_ssl_handshake();
    void reset_link() noexcept;
    void reset_owned_av() noexcept;
    [[nodiscard]] core::Result<std::size_t>
    send_payload(const protocol::ServiceDiscoveryResponse &response);
    [[nodiscard]] core::Result<std::size_t>
    send_payload(const protocol::ChannelOpenResponse &response);
    [[nodiscard]] core::Result<std::size_t> send_encapsulated_ssl(std::vector<std::byte> bytes);
    [[nodiscard]] core::Result<std::size_t> send_frames(std::vector<std::byte> payload,
                                                        bool encrypted);
    [[nodiscard]] core::Result<void> forward_input(const input::NormalizedInput &event);
    [[nodiscard]] core::Result<void> send_service_payload(std::uint8_t channel,
                                                         std::vector<std::byte> payload,
                                                         transport::MessageKind kind =
                                                             transport::MessageKind::specific);
    [[nodiscard]] std::optional<std::uint8_t> input_channel() const noexcept;

    void note_state(std::string_view identity_key = {});
    void emit(std::string_view identity_key = {});
    enum class RecoveryStatus : std::uint8_t { pending, accepted, failed };
    void note_recovery(session::Event event, const core::Result<void> &outcome);
    void emit_recovery(const char *reason, RecoveryStatus status, std::optional<std::int64_t> code);
    void resolve_recovery(RecoveryStatus status, std::optional<std::int64_t> code);

    WiredSessionDeps deps_;
    WiredSessionConfig config_;
    session::Lifecycle lifecycle_;
    channels::ServiceRegistry registry_;
    std::vector<protocol::ServiceDescriptor> descriptors_;
    std::optional<channels::Negotiator> negotiator_;
    std::map<protocol::ChannelRole, protocol::MessageSink *> sinks_;
    std::map<protocol::ChannelRole, std::unique_ptr<channels::AvSink>> av_sinks_;
    transport::FrameStreamParser parser_{};
    transport::MessageReassembler reassembler_{};
    std::unique_ptr<InputForwarder> input_forwarder_{};
    std::optional<input::InputService> input_service_{};
    transport::Transport *transport_{nullptr};
    std::unique_ptr<transport::HandshakingCipher> cipher_{};
    AuthPhase auth_phase_{AuthPhase::awaiting_version};
    bool ssl_active_{false};
    core::SessionId session_id_{core::SessionId{1}};
    session::State last_state_{session::State::disconnected};
    std::uint64_t sequence_{0};
    std::uint64_t diagnostics_emitted_{0};
    std::uint64_t input_sends_{0};
    std::uint64_t input_send_failures_{0};
    std::uint64_t channel_opens_{0};
    std::uint64_t unhandled_controls_{0};
    std::uint64_t channel_closes_{0};
    std::optional<RejectedFrame> last_reject_{};
    std::uint64_t version_responses_{0};
    std::uint64_t pings_answered_{0};
    std::uint64_t encapsulated_ssls_{0};
    std::uint64_t auth_completes_{0};
    std::uint64_t reconnect_attempts_{0};
    std::uint64_t recovery_failures_{0};
    bool recovery_in_flight_{false};
    const char *recovery_reason_{"link lost"};
    std::string last_identity_key_{};
};

} // namespace aa::app
