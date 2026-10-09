// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <aa/channels/Negotiator.hpp>
#include <aa/core/Result.hpp>
#include <aa/replay/Fixture.hpp>
#include <aa/session/Clock.hpp>
#include <aa/session/Lifecycle.hpp>
#include <aa/trust/Trust.hpp>
#include <aa/transport/Framing.hpp>
#include <aa/transport/Frames.hpp>
#include <aa/transport/Transport.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace aa::replay {

// ONE replay player (task 20). It runs a loaded fixture record by record on an
// injected fake monotonic clock and passes every step through the real
// accepted-core APIs - task-17 Lifecycle (admission, deadlines, cancel, fresh
// reconnect transport), the real transport frame parser stack
// (FrameStreamParser/decode_frame/MessageReassembler), task-18
// Negotiator start/open/dispatch/reset with negotiated routing, the pinned
// protocol encoders for expected responses, and the typed audio sink. No state
// or routing rule is re-simulated here.
//
// Determinism: record timestamps drive the clock; observed states, sink
// outcomes and matched sends form the output trace compared against the
// fixture's explicit expectation. Any script step refusal, parse failure or
// send mismatch aborts run() with that typed error and keeps the partial
// trace visible in observed().

// Player-driven fake monotonic clock (the only clock the player arms).
class ReplayClock final : public session::MonotonicClock {
public:
    void set(core::Nanoseconds t) noexcept { now_ = t; }
    [[nodiscard]] core::Nanoseconds now() const noexcept override { return now_; }

private:
    core::Nanoseconds now_{};
};

// Scripted transport implementing the real transport contract on fixture
// records: receive() serves the next inbound frame (transport_closed at end,
// cancelled once the bound token fires, and every failure closes it), send()
// matches the next expected outbound frame and reports a mismatch as a typed
// invalid_argument. Byte/count bounds come from the fixture admission limits.
class ReplayTransport final : public transport::Transport {
public:
    // Inbound frames to serve and outbound frames to match, in fixture order.
    struct Script final {
        std::vector<std::vector<std::byte>> inbound{};
        std::vector<std::vector<std::byte>> outbound{};
        std::size_t inbound_pos{};
        std::size_t outbound_pos{};
        std::int64_t matched_sends{};
    };

    explicit ReplayTransport(Script& script) noexcept : script_(&script) {}

    [[nodiscard]] transport::Kind kind() const noexcept override {
        return transport::Kind::usb_aoa;
    }
    core::Result<void> open(core::CancellationToken cancellation) override;
    core::Result<void> send(std::span<const std::byte> frame) override;
    core::Result<std::vector<std::byte>> receive() override;
    void close() noexcept override;

    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    Script* script_;
    std::optional<core::CancellationToken> token_{};
    bool open_{false};
    bool closed_{false};
};

class ReplayPlayer final {
public:
    struct Deps final {
        const trust::TrustStore& trust;
    };

    ReplayPlayer(Fixture fixture, Deps deps);
    ReplayPlayer(const ReplayPlayer&) = delete;
    ReplayPlayer& operator=(const ReplayPlayer&) = delete;
    ReplayPlayer(ReplayPlayer&&) = delete;
    ReplayPlayer& operator=(ReplayPlayer&&) = delete;

    // Channel sinks receive the negotiated MessageView deliveries; the audio
    // sink receives typed PcmChunk writes. Optional: an unregistered sink
    // still yields observed outcomes, except audio records which then fail
    // closed with audio_sink_unavailable.
    core::Result<void> register_sink(protocol::ChannelRole role, protocol::MessageSink& sink);
    core::Result<void> register_audio_sink(audio::AudioSink& sink);

    // Validates the fixture once, then runs every record in order. A clean
    // end requires the observed trace to equal the fixture's expected trace
    // exactly; any script refusal, parse failure, send mismatch or output
    // mismatch aborts with that typed error (observed() keeps the partial
    // trace). One-shot: use a fresh player to run again.
    [[nodiscard]] core::Result<Expectation> run();

    [[nodiscard]] const Expectation& observed() const noexcept { return observed_; }
    [[nodiscard]] session::State state() const noexcept { return lifecycle_.state(); }
    [[nodiscard]] std::optional<Error> failure() const { return lifecycle_.failure(); }
    [[nodiscard]] core::CancellationToken token() const noexcept { return lifecycle_.token(); }

    // Active transport handle (replaced by attach_fresh) for seam tests:
    // end/cancel/send-mismatch typed errors are observed here.
    [[nodiscard]] transport::Transport& transport() noexcept { return *active_; }
    void request_stop() noexcept { lifecycle_.request_stop(); }

private:
    // Observer registered with the real registry; records outcomes and
    // forwards to the user sink.
    class Observer final : public protocol::MessageSink {
    public:
        core::Result<void> on_message(const protocol::MessageView& message) override;
        protocol::ChannelRole role{protocol::ChannelRole::video};
        Expectation* observed{};
        protocol::MessageSink* forward{};
    };

    [[nodiscard]] core::Result<void> step_session(const SessionRecord& step);
    [[nodiscard]] core::Result<void> step_transport(const TransportRecord& record,
                                                    std::uint64_t sequence,
                                                    core::Nanoseconds at);
    [[nodiscard]] core::Result<void> step_video(const VideoRecord& record);
    [[nodiscard]] core::Result<void> step_audio(const AudioRecord& record);
    [[nodiscard]] core::Result<void> step_input(const InputRecord& record);
    [[nodiscard]] core::Result<void> route_message(const transport::Message& message,
                                                   std::uint64_t sequence,
                                                   core::Nanoseconds at);
    [[nodiscard]] core::Result<void> handle_control(std::span<const std::byte> payload);
    [[nodiscard]] core::Result<void> send_payload(const protocol::ServiceDiscoveryResponse& response);
    [[nodiscard]] core::Result<void> send_payload(const protocol::ChannelOpenResponse& response);
    [[nodiscard]] core::Result<void> send_frames(std::vector<std::byte> payload);
    [[nodiscard]] core::Result<std::uint8_t> service_of(protocol::ChannelRole role) const;
    [[nodiscard]] core::Result<void> guard_delivery() const;
    void teardown_channels();
    void observe_teardown();
    void note_state();

    Fixture fixture_;
    Deps deps_;
    ReplayClock clock_{};
    ReplayTransport::Script script_{};
    std::unique_ptr<ReplayTransport> transport_a_;
    std::unique_ptr<ReplayTransport> transport_b_;
    ReplayTransport* active_;
    session::Lifecycle lifecycle_;
    channels::ServiceRegistry registry_{};
    std::optional<channels::Negotiator> negotiator_{};
    std::map<protocol::ChannelRole, Observer> observers_{};
    std::map<protocol::ChannelRole, protocol::MessageSink*> user_sinks_{};
    audio::AudioSink* audio_sink_{};
    transport::FrameStreamParser parser_{};
    transport::MessageReassembler reassembler_{};
    Expectation observed_{};
    bool ran_{false};
};

} // namespace aa::replay
