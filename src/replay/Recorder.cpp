// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/replay/Recorder.hpp>

#include <aa/ipc/VideoSocket.hpp>
#include <aa/transport/Frames.hpp>

#include "FixtureValidate.hpp"

#include <cstdint>
#include <set>
#include <string>
#include <variant>

namespace aa::replay {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }
constexpr Error oversize() { return Error{ErrorCode::transport_oversize_frame}; }

struct TextOf {
    std::string operator()(const std::string& value) const { return value; }
    std::string operator()(std::int64_t value) const { return std::to_string(value); }
    std::string operator()(double value) const { return std::to_string(value); }
    std::string operator()(bool value) const { return value ? "true" : "false"; }
};

} // namespace

core::Result<void> Recorder::set_metadata(const diagnostics::EventField& field) {
    if (field.key.empty() || field.key.size() > kMaxMetadataTextBytes) {
        return malformed();
    }
    if (metadata_fields_.size() >= kMaxMetadataFields) {
        return malformed();
    }
    for (const auto& existing : metadata_fields_) {
        if (existing.key == field.key) {
            return malformed();
        }
    }
    if (const auto* text = std::get_if<std::string>(&field.value);
        text != nullptr && text->size() > kMaxMetadataTextBytes) {
        return malformed();
    }
    metadata_fields_.push_back(field);
    return {};
}

std::set<protocol::ChannelRole> Recorder::service_roles() const {
    std::set<protocol::ChannelRole> roles;
    for (const auto& service : fixture_.services) {
        roles.insert(service.role);
    }
    return roles;
}

core::Result<void> Recorder::set_services(std::vector<ServiceEntry> services) {
    if (const auto roles = detail::validate_service_entries(services); !roles) {
        return roles.error();
    }
    if (!fixture_.expect.states.empty()) {
        std::set<protocol::ChannelRole> next;
        for (const auto& service : services) {
            next.insert(service.role);
        }
        if (const auto expect = detail::validate_expectation(fixture_.expect, next); !expect) {
            return expect.error();
        }
    }
    fixture_.services = std::move(services);
    return {};
}

core::Result<void> Recorder::admit(core::Nanoseconds t, std::size_t payload_bytes) {
    return detail::admit_record(budget_, t, payload_bytes);
}

core::Result<void> Recorder::record_session(core::Nanoseconds t, const SessionRecord& step) {
    switch (step.op) {
    case SessionOp::event:
        if (const auto event = detail::validate_session_event(step.event); !event) {
            return event.error();
        }
        break;
    case SessionOp::request_stop:
    case SessionOp::tick:
    case SessionOp::attach_fresh:
    case SessionOp::reset_channels: break;
    case SessionOp::phone_discovered:
    case SessionOp::authorize_pairing:
    case SessionOp::confirm_pairing:
        if (const auto key = detail::validate_phone_key(step.phone_key); !key) {
            return key.error();
        }
        break;
    case SessionOp::fail:
        if (domain_of(step.fail_code) != ErrorDomain::core
            && domain_of(step.fail_code) != ErrorDomain::session) {
            return malformed();
        }
        break;
    }
    if (const auto admitted = admit(t, 0); !admitted) {
        return admitted.error();
    }
    Record record;
    record.kind = RecordKind::session;
    record.t = t;
    record.body = step;
    fixture_.records.push_back(std::move(record));
    return {};
}

core::Result<void> Recorder::record_transport(core::Nanoseconds t, Direction dir,
                                              std::span<const std::byte> frame) {
    if (const auto valid = detail::validate_direction(dir); !valid) {
        return valid.error();
    }
    if (frame.empty() || frame.size() > transport::kMaxWireFrameBytes) {
        return oversize();
    }
    if (const auto admitted = admit(t, frame.size()); !admitted) {
        return admitted.error();
    }
    Record record;
    record.kind = RecordKind::transport;
    record.t = t;
    TransportRecord body;
    body.dir = dir;
    body.frame.assign(frame.begin(), frame.end());
    record.body = std::move(body);
    fixture_.records.push_back(std::move(record));
    return {};
}

core::Result<void> Recorder::record_video(core::Nanoseconds t, const VideoRecord& video) {
    if (const auto id = detail::validate_wire_id(video.frame); !id) {
        return id.error();
    }
    if (const auto id = detail::validate_wire_id(video.sequence); !id) {
        return id.error();
    }
    if (video.payload.empty() || video.payload.size() > ipc::kMaxAccessUnitBytes) {
        return oversize();
    }
    if (const auto time = detail::validate_media_time(video.timestamp); !time) {
        return time.error();
    }
    if (const auto admitted = admit(t, video.payload.size()); !admitted) {
        return admitted.error();
    }
    Record record;
    record.kind = RecordKind::video;
    record.t = t;
    record.body = video;
    fixture_.records.push_back(std::move(record));
    return {};
}

core::Result<void> Recorder::record_audio(core::Nanoseconds t, const AudioRecord& audio) {
    if (const auto role = detail::validate_audio_role(audio.role); !role) {
        return role.error();
    }
    if (audio.pcm.empty() || audio.pcm.size() > kMaxAudioPayloadBytes) {
        return oversize();
    }
    if (const auto time = detail::validate_media_time(audio.timestamp); !time) {
        return time.error();
    }
    if (const auto format = detail::validate_audio_format(audio.format); !format) {
        return format.error();
    }
    if (const auto admitted = admit(t, audio.pcm.size()); !admitted) {
        return admitted.error();
    }
    Record record;
    record.kind = RecordKind::audio;
    record.t = t;
    record.body = audio;
    fixture_.records.push_back(std::move(record));
    return {};
}

core::Result<void> Recorder::record_input(core::Nanoseconds t, const InputRecord& input) {
    if (const auto id = detail::validate_wire_id(input.sequence); !id) {
        return id.error();
    }
    if (input.payload.empty() || input.payload.size() > kMaxInputPayloadBytes) {
        return oversize();
    }
    if (const auto time = detail::validate_media_time(input.timestamp); !time) {
        return time.error();
    }
    if (const auto admitted = admit(t, input.payload.size()); !admitted) {
        return admitted.error();
    }
    Record record;
    record.kind = RecordKind::input;
    record.t = t;
    record.body = input;
    fixture_.records.push_back(std::move(record));
    return {};
}

core::Result<void> Recorder::set_expectation(Expectation expected) {
    if (const auto expect = detail::validate_expectation(expected, service_roles()); !expect) {
        return expect.error();
    }
    fixture_.expect = std::move(expected);
    return {};
}

core::Result<ExportedFixture> Recorder::export_fixture() const {
    std::vector<diagnostics::EventField> fields;
    fields.reserve(metadata_fields_.size() + 2);
    bool have_frames = false;
    bool have_bytes = false;
    for (const auto& field : metadata_fields_) {
        const std::string text = std::visit(TextOf{}, field.value);
        have_frames = have_frames || field.key == "frames";
        have_bytes = have_bytes || field.key == "bytes";
        if (field.sensitivity == diagnostics::Sensitivity::identifier
            || has_identifier_shape(text)) {
            fields.push_back(
                diagnostics::EventField::text(field.key, std::string{"[redacted]"}));
        } else {
            fields.push_back(field);
        }
    }
    const auto records = static_cast<std::int64_t>(budget_.records);
    if (!have_frames) {
        fields.push_back(diagnostics::EventField::count("frames", records));
    }
    if (!have_bytes) {
        fields.push_back(diagnostics::EventField::count(
            "bytes", static_cast<std::int64_t>(budget_.payload_bytes)));
    }
    diagnostics::Event event;
    event.kind = diagnostics::EventKind::transport_stats;
    event.session = core::SessionId{1};
    event.monotonic_time = core::Nanoseconds{0};
    event.fields = std::move(fields);
    Fixture sealed = fixture_;
    sealed.metadata = fixedredact_metadata(diagnostics::to_json(event));
    return aa::replay::export_fixture(sealed);
}

} // namespace aa::replay
