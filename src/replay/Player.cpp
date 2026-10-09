// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/replay/Player.hpp>

#include <aa/protocol/Messages.hpp>

#include "FixtureValidate.hpp"

#include <cstdint>
#include <utility>

namespace aa::replay {
namespace {

constexpr Error mismatch() { return Error{ErrorCode::invalid_argument}; }

std::vector<protocol::ServiceDescriptor> descriptors_of(const Fixture& fixture) {
    std::vector<protocol::ServiceDescriptor> descriptors;
    descriptors.reserve(fixture.services.size());
    for (const auto& service : fixture.services) {
        descriptors.push_back({service.id, service.role, service.configuration});
    }
    return descriptors;
}

} // namespace

ReplayPlayer::ReplayPlayer(Fixture fixture, Deps deps)
    : fixture_(std::move(fixture)), deps_(deps), transport_a_(std::make_unique<ReplayTransport>(
                                                     script_)),
      transport_b_(std::make_unique<ReplayTransport>(script_)), active_(transport_a_.get()),
      lifecycle_(session::LifecycleConfig{}, clock_, deps_.trust, active_) {}

core::Result<void> ReplayPlayer::register_sink(protocol::ChannelRole role,
                                               protocol::MessageSink& sink) {
    if (user_sinks_.contains(role)) {
        return mismatch();
    }
    user_sinks_.emplace(role, &sink);
    return {};
}

core::Result<void> ReplayPlayer::register_audio_sink(audio::AudioSink& sink) {
    if (audio_sink_ != nullptr) {
        return mismatch();
    }
    audio_sink_ = &sink;
    return {};
}

void ReplayPlayer::note_state() {
    const std::string name{session::to_string(lifecycle_.state())};
    if (observed_.states.empty() || observed_.states.back() != name) {
        observed_.states.push_back(name);
    }
}

core::Result<std::uint8_t> ReplayPlayer::service_of(protocol::ChannelRole role) const {
    for (const auto& service : fixture_.services) {
        if (service.role == role && service.id) {
            return static_cast<std::uint8_t>(service.id.value);
        }
    }
    return mismatch();
}

core::Result<void> ReplayPlayer::guard_delivery() const {
    if (lifecycle_.token().stop_requested()) {
        return Error{ErrorCode::cancelled};
    }
    return {};
}

void ReplayPlayer::teardown_channels() {
    if (negotiator_) {
        negotiator_->reset();
    }
    parser_ = transport::FrameStreamParser{};
    reassembler_ = transport::MessageReassembler{};
}

void ReplayPlayer::observe_teardown() {
    // The lifecycle closes the attached transport exactly when the session or
    // link is terminal (retire/cleanup); that terminal close is the owner's
    // teardown signal - no duplicate lifecycle policy is re-decided here.
    if (active_->closed()) {
        teardown_channels();
    }
}

core::Result<void> ReplayPlayer::step_session(const SessionRecord& step) {
    const trust::PhoneIdentity identity{step.phone_key};
    switch (step.op) {
    case SessionOp::event: return lifecycle_.apply(step.event);
    case SessionOp::phone_discovered: return lifecycle_.phone_discovered(identity);
    case SessionOp::authorize_pairing: return lifecycle_.authorize_pairing(identity);
    case SessionOp::confirm_pairing: return lifecycle_.confirm_pairing(identity);
    case SessionOp::fail: return lifecycle_.fail(Error{step.fail_code});
    case SessionOp::request_stop:
        lifecycle_.request_stop();
        return {};
    case SessionOp::tick:
        lifecycle_.tick();
        return {};
    case SessionOp::attach_fresh:
        teardown_channels();
        active_ = transport_b_.get();
        lifecycle_.attach_transport(active_);
        return {};
    case SessionOp::reset_channels:
        teardown_channels();
        return {};
    }
    return mismatch();
}

core::Result<void> ReplayPlayer::step_transport(const TransportRecord& record,
                                                std::uint64_t sequence, core::Nanoseconds at) {
    if (record.dir == Direction::outbound) {
        return {};
    }
    auto frame = active_->receive();
    if (!frame) {
        return frame.error();
    }
    auto chunks = parser_.feed(frame.value());
    if (!chunks) {
        return chunks.error();
    }
    for (const auto& chunk : chunks.value()) {
        auto message = reassembler_.push(chunk);
        if (!message) {
            return message.error();
        }
        if (message.value().has_value()) {
            if (auto routed = route_message(*message.value(), sequence, at); !routed) {
                return routed.error();
            }
        }
    }
    return {};
}

core::Result<void> ReplayPlayer::step_video(const VideoRecord& record) {
    if (auto live = guard_delivery(); !live) {
        return live.error();
    }
    auto service = service_of(protocol::ChannelRole::video);
    if (!service) {
        return service.error();
    }
    return negotiator_->dispatch(protocol::WireMessageView{
        service.value(), record.sequence, record.timestamp, record.payload});
}

core::Result<void> ReplayPlayer::step_input(const InputRecord& record) {
    if (auto live = guard_delivery(); !live) {
        return live.error();
    }
    auto service = service_of(protocol::ChannelRole::input);
    if (!service) {
        return service.error();
    }
    return negotiator_->dispatch(protocol::WireMessageView{
        service.value(), record.sequence, record.timestamp, record.payload});
}

core::Result<void> ReplayPlayer::step_audio(const AudioRecord& record) {
    if (auto live = guard_delivery(); !live) {
        return live.error();
    }
    if (audio_sink_ == nullptr) {
        return Error{ErrorCode::audio_sink_unavailable};
    }
    const audio::PcmChunk chunk{record.role, record.format, record.timestamp, record.pcm};
    if (auto written = audio_sink_->write(chunk); !written) {
        return written.error();
    }
    observed_.audio.push_back(
        AudioOutcome{record.role, record.format, record.timestamp, record.pcm});
    return {};
}

core::Result<Expectation> ReplayPlayer::run() {
    if (ran_) {
        return mismatch();
    }
    ran_ = true;
    if (const auto valid = detail::validate_fixture(fixture_); !valid) {
        return valid.error();
    }
    for (const auto& record : fixture_.records) {
        if (record.kind != RecordKind::transport) {
            continue;
        }
        const auto& body = std::get<TransportRecord>(record.body);
        if (body.dir == Direction::inbound) {
            script_.inbound.push_back(body.frame);
        } else {
            script_.outbound.push_back(body.frame);
        }
    }
    auto support = channels::SupportConfiguration::create(descriptors_of(fixture_));
    if (!support) {
        return support.error();
    }
    negotiator_.emplace(std::move(support).value(), registry_);
    for (const auto& service : fixture_.services) {
        const auto [slot, inserted] = observers_.try_emplace(service.role);
        (void)inserted;
        slot->second.role = service.role;
        slot->second.observed = &observed_;
        const auto user = user_sinks_.find(service.role);
        slot->second.forward = user == user_sinks_.end() ? nullptr : user->second;
        if (auto registered = registry_.register_sink(service.role, slot->second); !registered) {
            return registered.error();
        }
    }
    observed_ = Expectation{};
    observed_.states.push_back(std::string{session::to_string(session::State::disconnected)});

    core::Result<void> outcome{};
    for (std::size_t index = 0; index < fixture_.records.size() && outcome; ++index) {
        const auto& record = fixture_.records[index];
        clock_.set(record.t);
        const auto sequence = static_cast<std::uint64_t>(index);
        switch (record.kind) {
        case RecordKind::session:
            outcome = step_session(std::get<SessionRecord>(record.body));
            note_state();
            break;
        case RecordKind::transport:
            outcome = step_transport(std::get<TransportRecord>(record.body), sequence, record.t);
            break;
        case RecordKind::video:
            outcome = step_video(std::get<VideoRecord>(record.body));
            break;
        case RecordKind::audio:
            outcome = step_audio(std::get<AudioRecord>(record.body));
            break;
        case RecordKind::input:
            outcome = step_input(std::get<InputRecord>(record.body));
            break;
        }
        observe_teardown();
    }
    observed_.sends = script_.matched_sends;
    if (audio_sink_ != nullptr) {
        audio_sink_->flush();
    }
    if (!outcome) {
        return outcome.error();
    }
    if (script_.outbound_pos != script_.outbound.size()) {
        return mismatch();
    }
    if (!(observed_ == fixture_.expect)) {
        return mismatch();
    }
    return observed_;
}

} // namespace aa::replay
