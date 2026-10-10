// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureValidate.hpp"

#include "Json.hpp"

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

namespace aa::replay {
namespace {

// Canonical body grammar (the hashed region is exactly these bytes):
//   {"provenance":"synthetic","metadata":"<line>","services":[S...],"records":[R...],"expect":E}
//   S: {"id":N,"role":"<role>","video":[[w,h,fps]...]} | {"id":N,"role":"<role>","audio":[[rate,bits,ch]...]}
//      | {"id":N,"role":"<role>","buttons":[N...]} | {"id":N,"role":"<role>","none":true}
//   R: {"kind":"session","t":N,"op":"<op>",...} | {"kind":"transport","t":N,"dir":"in|out",
//      "opaque":"synthetic","frame":"<hex>"} | {"kind":"video","t":N,"frame":N,"sequence":N,
//      "ts":N,"idr":bool,"opaque":"synthetic","payload":"<hex>"} | {"kind":"audio","t":N,
//      "role":"<role>","rate":N,"bits":N,"channels":N,"ts":N,"opaque":"synthetic","pcm":"<hex>"}
//      | {"kind":"input","t":N,"sequence":N,"ts":N,"opaque":"synthetic","payload":"<hex>"}
//   E: {"states":[S...],"deliveries":[D...],"audio":[A...],"sends":N}
// Fixed key order, no whitespace, lowercase hex blobs. The file envelope is
//   {"schema":"aa-replay-fixture-v1","sha256":"<hex>","body":<canonical>}
// and sha256 covers "aa-replay-fixture-v1\n" || <canonical> bytes.

void write_int(std::string& out, std::int64_t value) { out += std::to_string(value); }

void write_bool(std::string& out, bool value) { out += value ? "true" : "false"; }

void write_string(std::string& out, std::string_view value) {
    detail::write_json_string(out, value);
}

void write_blob_key(std::string& out, std::string_view key, std::span<const std::byte> bytes) {
    out += ",\"";
    out += key;
    out += "\":";
    write_string(out, detail::to_hex(bytes));
}

void write_service(std::string& out, const ServiceEntry& service) {
    out += "{\"id\":";
    write_int(out, service.id.value);
    out += ",\"role\":";
    write_string(out, detail::role_name(service.role));
    std::visit(
        [&out](const auto& configuration) {
            using T = std::decay_t<decltype(configuration)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out += ",\"none\":true";
            } else if constexpr (std::is_same_v<T, protocol::VideoConfiguration>) {
                out += ",\"video\":[";
                bool first = true;
                for (const auto& profile : configuration.profiles) {
                    if (!first) {
                        out += ",";
                    }
                    first = false;
                    out += "[";
                    write_int(out, profile.width);
                    out += ",";
                    write_int(out, profile.height);
                    out += ",";
                    write_int(out, profile.fps);
                    out += "]";
                }
                out += "]";
            } else if constexpr (std::is_same_v<T, protocol::AudioConfiguration>) {
                out += ",\"audio\":[";
                bool first = true;
                for (const auto& profile : configuration.profiles) {
                    if (!first) {
                        out += ",";
                    }
                    first = false;
                    out += "[";
                    write_int(out, profile.sampling_rate);
                    out += ",";
                    write_int(out, profile.bits);
                    out += ",";
                    write_int(out, profile.channels);
                    out += "]";
                }
                out += "]";
            } else {
                out += ",\"buttons\":[";
                bool first = true;
                for (const auto keycode : configuration.keycodes) {
                    if (!first) {
                        out += ",";
                    }
                    first = false;
                    write_int(out, keycode);
                }
                out += "]";
            }
        },
        service.configuration);
    out += "}";
}

void write_session(std::string& out, const SessionRecord& step) {
    out += "\"op\":";
    write_string(out, detail::session_op_name(step.op));
    switch (step.op) {
    case SessionOp::event:
        out += ",\"event\":";
        write_string(out, session::to_string(step.event));
        return;
    case SessionOp::phone_discovered:
    case SessionOp::authorize_pairing:
    case SessionOp::confirm_pairing:
        out += ",\"phone\":";
        write_string(out, step.phone_key);
        return;
    case SessionOp::fail:
        out += ",\"code\":";
        write_int(out, static_cast<std::int64_t>(step.fail_code));
        return;
    case SessionOp::request_stop:
    case SessionOp::tick:
    case SessionOp::attach_fresh:
    case SessionOp::reset_channels: return;
    }
}

void write_record(std::string& out, const Record& record) {
    out += "{\"kind\":";
    write_string(out, to_string(record.kind));
    out += ",\"t\":";
    write_int(out, record.t.count);
    switch (record.kind) {
    case RecordKind::session:
        out += ",";
        write_session(out, std::get<SessionRecord>(record.body));
        break;
    case RecordKind::transport: {
        const auto& body = std::get<TransportRecord>(record.body);
        out += ",\"dir\":";
        write_string(out, to_string(body.dir));
        out += ",\"opaque\":";
        write_string(out, kOpaquePolicy);
        write_blob_key(out, "frame", body.frame);
        break;
    }
    case RecordKind::video: {
        const auto& body = std::get<VideoRecord>(record.body);
        out += ",\"frame\":";
        write_int(out, static_cast<std::int64_t>(body.frame));
        out += ",\"sequence\":";
        write_int(out, static_cast<std::int64_t>(body.sequence));
        out += ",\"ts\":";
        write_int(out, body.timestamp.count);
        out += ",\"idr\":";
        write_bool(out, body.idr);
        out += ",\"opaque\":";
        write_string(out, kOpaquePolicy);
        write_blob_key(out, "payload", body.payload);
        break;
    }
    case RecordKind::audio: {
        const auto& body = std::get<AudioRecord>(record.body);
        out += ",\"role\":";
        write_string(out, detail::audio_role_name(body.role));
        out += ",\"rate\":";
        write_int(out, body.format.sample_rate_hz);
        out += ",\"bits\":";
        write_int(out, body.format.bits_per_sample);
        out += ",\"channels\":";
        write_int(out, body.format.channels);
        out += ",\"ts\":";
        write_int(out, body.timestamp.count);
        out += ",\"opaque\":";
        write_string(out, kOpaquePolicy);
        write_blob_key(out, "pcm", body.pcm);
        break;
    }
    case RecordKind::input: {
        const auto& body = std::get<InputRecord>(record.body);
        out += ",\"sequence\":";
        write_int(out, static_cast<std::int64_t>(body.sequence));
        out += ",\"ts\":";
        write_int(out, body.timestamp.count);
        out += ",\"opaque\":";
        write_string(out, kOpaquePolicy);
        write_blob_key(out, "payload", body.payload);
        break;
    }
    }
    out += "}";
}

void write_delivery(std::string& out, const DeliveryOutcome& delivery) {
    out += "{\"role\":";
    write_string(out, detail::role_name(delivery.role));
    out += ",\"sequence\":";
    write_int(out, static_cast<std::int64_t>(delivery.sequence));
    out += ",\"ts\":";
    write_int(out, delivery.timestamp.count);
    write_blob_key(out, "payload", delivery.payload);
    out += "}";
}

void write_audio_outcome(std::string& out, const AudioOutcome& outcome) {
    out += "{\"role\":";
    write_string(out, detail::audio_role_name(outcome.role));
    out += ",\"rate\":";
    write_int(out, outcome.format.sample_rate_hz);
    out += ",\"bits\":";
    write_int(out, outcome.format.bits_per_sample);
    out += ",\"channels\":";
    write_int(out, outcome.format.channels);
    out += ",\"ts\":";
    write_int(out, outcome.timestamp.count);
    write_blob_key(out, "pcm", outcome.pcm);
    out += "}";
}

std::string encode_body(const Fixture& fixture) {
    std::string out;
    out += "{\"provenance\":";
    write_string(out, kSyntheticProvenance);
    out += ",\"metadata\":";
    write_string(out, fixture.metadata);
    out += ",\"services\":[";
    for (std::size_t i = 0; i < fixture.services.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        write_service(out, fixture.services[i]);
    }
    out += "],\"records\":[";
    for (std::size_t i = 0; i < fixture.records.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        write_record(out, fixture.records[i]);
    }
    out += "],\"expect\":{\"states\":[";
    for (std::size_t i = 0; i < fixture.expect.states.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        write_string(out, fixture.expect.states[i]);
    }
    out += "],\"deliveries\":[";
    for (std::size_t i = 0; i < fixture.expect.deliveries.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        write_delivery(out, fixture.expect.deliveries[i]);
    }
    out += "],\"audio\":[";
    for (std::size_t i = 0; i < fixture.expect.audio.size(); ++i) {
        if (i > 0) {
            out += ",";
        }
        write_audio_outcome(out, fixture.expect.audio[i]);
    }
    out += "],\"sends\":";
    write_int(out, fixture.expect.sends);
    out += "}}";
    return out;
}

} // namespace

namespace detail {

std::string encode_canonical_body(const Fixture& fixture) { return encode_body(fixture); }

} // namespace detail
} // namespace aa::replay
