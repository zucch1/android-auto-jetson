// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureValidate.hpp"

#include <aa/ipc/VideoSocket.hpp>
#include <aa/replay/Schema.hpp>
#include <aa/transport/Frames.hpp>

#include "Json.hpp"
#include "MetadataGate.hpp"

#include <cstdint>
#include <string>
#include <variant>

namespace aa::replay::detail {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }
constexpr Error oversize() { return Error{ErrorCode::transport_oversize_frame}; }

struct Totals final {
    std::size_t records{};
    std::size_t payload_bytes{};
};

std::size_t payload_bytes(const Record& record) {
    struct Size {
        std::size_t operator()(const SessionRecord&) const { return 0; }
        std::size_t operator()(const TransportRecord& r) const { return r.frame.size(); }
        std::size_t operator()(const VideoRecord& r) const { return r.payload.size(); }
        std::size_t operator()(const AudioRecord& r) const { return r.pcm.size(); }
        std::size_t operator()(const InputRecord& r) const { return r.payload.size(); }
    };
    return std::visit(Size{}, record.body);
}

core::Result<void> validate_session(const SessionRecord& step) {
    switch (step.op) {
    case SessionOp::event: return validate_session_event(step.event);
    case SessionOp::phone_discovered:
    case SessionOp::authorize_pairing:
    case SessionOp::confirm_pairing: return validate_phone_key(step.phone_key);
    case SessionOp::fail:
        if (domain_of(step.fail_code) != ErrorDomain::core
            && domain_of(step.fail_code) != ErrorDomain::session) {
            return malformed();
        }
        return {};
    case SessionOp::request_stop:
    case SessionOp::tick:
    case SessionOp::attach_fresh:
    case SessionOp::reset_channels: return {};
    }
    return malformed();
}

core::Result<void> validate_record(const Record& record) {
    if (const auto admitted = validate_media_time(record.t); !admitted) {
        return admitted.error();
    }
    switch (record.kind) {
    case RecordKind::session: {
        const auto* step = std::get_if<SessionRecord>(&record.body);
        return step == nullptr ? malformed() : validate_session(*step);
    }
    case RecordKind::transport: {
        const auto* body = std::get_if<TransportRecord>(&record.body);
        if (body == nullptr) {
            return malformed();
        }
        if (const auto dir = validate_direction(body->dir); !dir) {
            return dir.error();
        }
        if (body->frame.empty() || body->frame.size() > transport::kMaxWireFrameBytes) {
            return oversize();
        }
        return {};
    }
    case RecordKind::video: {
        const auto* body = std::get_if<VideoRecord>(&record.body);
        if (body == nullptr) {
            return malformed();
        }
        if (body->payload.empty() || body->payload.size() > ipc::kMaxAccessUnitBytes) {
            return oversize();
        }
        if (const auto id = validate_wire_id(body->frame); !id) {
            return id.error();
        }
        if (const auto id = validate_wire_id(body->sequence); !id) {
            return id.error();
        }
        return validate_media_time(body->timestamp);
    }
    case RecordKind::audio: {
        const auto* body = std::get_if<AudioRecord>(&record.body);
        if (body == nullptr) {
            return malformed();
        }
        if (body->pcm.empty() || body->pcm.size() > kMaxAudioPayloadBytes) {
            return oversize();
        }
        if (const auto role = validate_audio_role(body->role); !role) {
            return role.error();
        }
        if (const auto time = validate_media_time(body->timestamp); !time) {
            return time.error();
        }
        return validate_audio_format(body->format);
    }
    case RecordKind::input: {
        const auto* body = std::get_if<InputRecord>(&record.body);
        if (body == nullptr) {
            return malformed();
        }
        if (body->payload.empty() || body->payload.size() > kMaxInputPayloadBytes) {
            return oversize();
        }
        if (const auto id = validate_wire_id(body->sequence); !id) {
            return id.error();
        }
        return validate_media_time(body->timestamp);
    }
    }
    return malformed();
}

core::Result<void> validate_metadata_counters(const std::string& metadata, Totals totals) {
    const auto parsed = parse_json(metadata);
    if (!parsed || !parsed.value().is_object()) {
        return malformed();
    }
    for (const auto& member : parsed.value().as_object()) {
        std::int64_t declared = 0;
        if (!member.second.int_value(declared) || declared < 0) {
            continue;
        }
        if (member.first == "frames" && declared != static_cast<std::int64_t>(totals.records)) {
            return malformed();
        }
        if (member.first == "bytes"
            && declared != static_cast<std::int64_t>(totals.payload_bytes)) {
            return malformed();
        }
    }
    return {};
}

} // namespace

core::Result<void> validate_fixture(const Fixture& fixture) {
    if (const auto provenance = validate_provenance(fixture.provenance); !provenance) {
        return provenance.error();
    }
    const auto roles = validate_service_entries(fixture.services);
    if (!roles) {
        return roles.error();
    }

    if (fixture.records.empty()) {
        return malformed();
    }
    AdmissionBudget budget;
    std::int64_t outbound = 0;
    for (const auto& record : fixture.records) {
        if (const auto valid = validate_record(record); !valid) {
            return valid.error();
        }
        if (const auto admitted = admit_record(budget, record.t, payload_bytes(record));
            !admitted) {
            return admitted.error();
        }
        if (record.kind == RecordKind::transport) {
            const auto* body = std::get_if<TransportRecord>(&record.body);
            if (body != nullptr && body->dir == Direction::outbound) {
                ++outbound;
            }
        }
    }

    if (const auto expect = validate_expectation(fixture.expect, roles.value()); !expect) {
        return expect.error();
    }
    if (fixture.expect.sends != outbound) {
        return malformed();
    }

    if (const auto gate = metadata_gate(fixture.metadata); !gate) {
        return gate.error();
    }
    return validate_metadata_counters(fixture.metadata,
                                      Totals{budget.records, budget.payload_bytes});
}

} // namespace aa::replay::detail
