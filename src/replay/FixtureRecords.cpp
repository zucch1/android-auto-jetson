// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureParse.hpp"

#include "FieldAccess.hpp"

#include <aa/replay/Schema.hpp>

namespace aa::replay::detail {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }

core::Result<Record> parse_transport(const JsonValue& value) {
    if (const auto keys = expect_keys(value, {"kind", "t", "dir", "opaque", "frame"}); !keys) {
        return keys.error();
    }
    const auto dir = string_of(field(value, "dir"));
    if (!dir) {
        return dir.error();
    }
    TransportRecord body;
    if (dir.value() == "in") {
        body.dir = Direction::inbound;
    } else if (dir.value() == "out") {
        body.dir = Direction::outbound;
    } else {
        return malformed();
    }
    if (const auto policy = policy_ok(field(value, "opaque")); !policy) {
        return policy.error();
    }
    if (const auto blob = blob_of(field(value, "frame"), body.frame); !blob) {
        return blob.error();
    }
    Record record;
    record.kind = RecordKind::transport;
    record.body = std::move(body);
    return record;
}

core::Result<Record> parse_video(const JsonValue& value) {
    if (const auto keys = expect_keys(value, {"kind", "t", "frame", "sequence", "ts", "idr",
                                              "opaque", "payload"});
        !keys) {
        return keys.error();
    }
    if (!field(value, "idr").is_bool()) {
        return malformed();
    }
    VideoRecord body;
    const auto frame = u64_of(field(value, "frame"));
    const auto sequence = u64_of(field(value, "sequence"));
    const auto ts = int_of(field(value, "ts"));
    if (!frame || !sequence || !ts) {
        return malformed();
    }
    body.frame = frame.value();
    body.sequence = sequence.value();
    body.timestamp = core::Nanoseconds{ts.value()};
    body.idr = field(value, "idr").as_bool();
    if (const auto policy = policy_ok(field(value, "opaque")); !policy) {
        return policy.error();
    }
    if (const auto blob = blob_of(field(value, "payload"), body.payload); !blob) {
        return blob.error();
    }
    Record record;
    record.kind = RecordKind::video;
    record.body = std::move(body);
    return record;
}

core::Result<Record> parse_audio(const JsonValue& value) {
    if (const auto keys = expect_keys(value, {"kind", "t", "role", "rate", "bits", "channels",
                                              "ts", "opaque", "pcm"});
        !keys) {
        return keys.error();
    }
    AudioRecord body;
    const auto role = audio_role_of(field(value, "role"));
    const auto rate = u32_of(field(value, "rate"));
    const auto bits = u8_of(field(value, "bits"));
    const auto channels = u8_of(field(value, "channels"));
    const auto ts = int_of(field(value, "ts"));
    if (!role || !rate || !bits || !channels || !ts) {
        return malformed();
    }
    body.role = role.value();
    body.format = audio::AudioFormat{rate.value(), channels.value(), bits.value()};
    body.timestamp = core::Nanoseconds{ts.value()};
    if (const auto policy = policy_ok(field(value, "opaque")); !policy) {
        return policy.error();
    }
    if (const auto blob = blob_of(field(value, "pcm"), body.pcm); !blob) {
        return blob.error();
    }
    Record record;
    record.kind = RecordKind::audio;
    record.body = std::move(body);
    return record;
}

core::Result<Record> parse_input(const JsonValue& value) {
    if (const auto keys = expect_keys(value, {"kind", "t", "sequence", "ts", "opaque",
                                              "payload"});
        !keys) {
        return keys.error();
    }
    InputRecord body;
    const auto sequence = u64_of(field(value, "sequence"));
    const auto ts = int_of(field(value, "ts"));
    if (!sequence || !ts) {
        return malformed();
    }
    body.sequence = sequence.value();
    body.timestamp = core::Nanoseconds{ts.value()};
    if (const auto policy = policy_ok(field(value, "opaque")); !policy) {
        return policy.error();
    }
    if (const auto blob = blob_of(field(value, "payload"), body.payload); !blob) {
        return blob.error();
    }
    Record record;
    record.kind = RecordKind::input;
    record.body = std::move(body);
    return record;
}

} // namespace

core::Result<Record> parse_record(const JsonValue& value) {
    if (!value.is_object()) {
        return malformed();
    }
    const auto kind_text = string_of(field(value, "kind"));
    const auto t = int_of(field(value, "t"));
    if (!kind_text || !t || t.value() < 0) {
        return malformed();
    }
    Record record;
    record.t = core::Nanoseconds{t.value()};
    if (kind_text.value() == "session") {
        record.kind = RecordKind::session;
        SessionRecord step;
        if (const auto parsed = parse_session_body(value, step); !parsed) {
            return parsed.error();
        }
        record.body = step;
        return record;
    }
    if (kind_text.value() == "transport") {
        auto parsed = parse_transport(value);
        if (!parsed) {
            return parsed.error();
        }
        parsed.value().t = record.t;
        return parsed.value();
    }
    if (kind_text.value() == "video") {
        auto parsed = parse_video(value);
        if (!parsed) {
            return parsed.error();
        }
        parsed.value().t = record.t;
        return parsed.value();
    }
    if (kind_text.value() == "audio") {
        auto parsed = parse_audio(value);
        if (!parsed) {
            return parsed.error();
        }
        parsed.value().t = record.t;
        return parsed.value();
    }
    if (kind_text.value() == "input") {
        auto parsed = parse_input(value);
        if (!parsed) {
            return parsed.error();
        }
        parsed.value().t = record.t;
        return parsed.value();
    }
    return malformed();
}

} // namespace aa::replay::detail
