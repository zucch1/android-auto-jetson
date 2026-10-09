// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureValidate.hpp"

#include "JsonLex.hpp"

#include <aa/channels/Negotiator.hpp>
#include <aa/ipc/VideoSocket.hpp>
#include <aa/protocol/Messages.hpp>
#include <aa/replay/Schema.hpp>

#include <string_view>

namespace aa::replay::detail {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }
constexpr Error oversize() { return Error{ErrorCode::transport_oversize_frame}; }

std::size_t expectation_payload(const Expectation& expect) {
    std::size_t total = 0;
    for (const auto& delivery : expect.deliveries) {
        total += delivery.payload.size();
    }
    for (const auto& outcome : expect.audio) {
        total += outcome.pcm.size();
    }
    return total;
}

} // namespace

core::Result<void> admit_record(AdmissionBudget& budget, core::Nanoseconds t,
                              std::size_t payload_bytes) {
    if (t.count < 0) {
        return malformed();
    }
    if (budget.records >= kMaxRecords) {
        return malformed();
    }
    if (budget.have_t) {
        if (t.count < budget.last_t) {
            return malformed();
        }
        if (t.count - budget.first_t > kMaxTraceNs) {
            return malformed();
        }
    }
    if (payload_bytes > kMaxTotalPayloadBytes - budget.payload_bytes) {
        return oversize();
    }
    if (!budget.have_t) {
        budget.first_t = t.count;
        budget.have_t = true;
    }
    budget.last_t = t.count;
    budget.records += 1;
    budget.payload_bytes += payload_bytes;
    return {};
}

core::Result<void> validate_audio_format(const audio::AudioFormat& format) {
    const protocol::AudioProfile profile{format.sample_rate_hz, format.bits_per_sample,
                                         format.channels};
    if (!protocol::valid_audio_profile(profile)) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_media_time(core::Nanoseconds timestamp) {
    if (timestamp.count < 0) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_phone_key(std::string_view key) {
    if (key.empty() || key.size() > kMaxPhoneKeyBytes || has_identifier_shape(key)
        || !is_valid_utf8(key)) {
        return malformed();
    }
    return {};
}

core::Result<void> validate_states(const Expectation& expect) {
    if (expect.states.empty() || expect.states.front() != "disconnected") {
        return malformed();
    }
    session::State previous = session::State::disconnected;
    bool first = true;
    for (const auto& name : expect.states) {
        if (name.size() > kMaxMetadataTextBytes) {
            return malformed();
        }
        auto current = session::State::disconnected;
        bool known = false;
        for (const session::State candidate :
             {session::State::disconnected, session::State::discovering, session::State::pairing,
              session::State::connecting, session::State::negotiating, session::State::active,
              session::State::degraded, session::State::reconnecting, session::State::stopping,
              session::State::failed}) {
            if (session::to_string(candidate) == name) {
                current = candidate;
                known = true;
                break;
            }
        }
        if (!known) {
            return malformed();
        }
        if (!first) {
            if (current == previous) {
                return malformed();
            }
            if (!session::is_legal_transition(previous, current)) {
                return Error{ErrorCode::session_illegal_transition};
            }
        }
        previous = current;
        first = false;
    }
    return {};
}

core::Result<void> validate_expectation_bounds(const Expectation& expect) {
    if (expect.states.size() > kMaxRecords + 1 || expect.deliveries.size() > kMaxRecords
        || expect.audio.size() > kMaxRecords) {
        return malformed();
    }
    if (expect.sends < 0) {
        return malformed();
    }
    for (const auto& delivery : expect.deliveries) {
        if (delivery.payload.empty() || delivery.payload.size() > ipc::kMaxAccessUnitBytes) {
            return oversize();
        }
        if (const auto id = validate_wire_id(delivery.sequence); !id) {
            return id.error();
        }
        if (const auto role = validate_channel_role(delivery.role); !role) {
            return role.error();
        }
        if (const auto time = validate_media_time(delivery.timestamp); !time) {
            return time.error();
        }
    }
    for (const auto& outcome : expect.audio) {
        if (outcome.pcm.empty() || outcome.pcm.size() > kMaxAudioPayloadBytes) {
            return oversize();
        }
        if (const auto role = validate_audio_role(outcome.role); !role) {
            return role.error();
        }
        if (const auto time = validate_media_time(outcome.timestamp); !time) {
            return time.error();
        }
        if (const auto format = validate_audio_format(outcome.format); !format) {
            return format.error();
        }
    }
    if (expectation_payload(expect) > kMaxTotalPayloadBytes) {
        return oversize();
    }
    return {};
}

core::Result<void> validate_expectation(const Expectation& expect,
                                        const std::set<protocol::ChannelRole>& roles) {
    if (const auto states = validate_states(expect); !states) {
        return states.error();
    }
    if (const auto bounded = validate_expectation_bounds(expect); !bounded) {
        return bounded.error();
    }
    for (const auto& delivery : expect.deliveries) {
        if (!roles.contains(delivery.role)) {
            return malformed();
        }
    }
    return {};
}

core::Result<std::set<protocol::ChannelRole>> validate_service_entries(
    const std::vector<ServiceEntry>& services) {
    if (services.size() > kMaxServices) {
        return malformed();
    }
    std::set<protocol::ChannelRole> roles;
    std::vector<protocol::ServiceDescriptor> descriptors;
    descriptors.reserve(services.size());
    for (const auto& service : services) {
        if (!service.id) {
            return malformed();
        }
        if (!roles.insert(service.role).second) {
            return malformed();
        }
        descriptors.push_back({service.id, service.role, service.configuration});
    }
    if (const auto support = channels::SupportConfiguration::create(std::move(descriptors));
        !support) {
        return support.error();
    }
    return roles;
}

} // namespace aa::replay::detail
