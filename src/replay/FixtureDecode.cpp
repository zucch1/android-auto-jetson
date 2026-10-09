// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/replay/Schema.hpp>

#include "FieldAccess.hpp"
#include "FixtureParse.hpp"
#include "FixtureValidate.hpp"
#include "Json.hpp"
#include "Sha256.hpp"

#include <string>
#include <string_view>

namespace aa::replay {
namespace {

using detail::JsonValue;

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }

constexpr std::string_view kFilePrefix = "{\"schema\":\"aa-replay-fixture-v1\",\"sha256\":\"";
constexpr std::string_view kFileSeparator = "\",\"body\":";

} // namespace

namespace detail {

core::Result<Expectation> parse_expectation(const JsonValue& value) {
    if (const auto keys = expect_keys(value, {"states", "deliveries", "audio", "sends"}); !keys) {
        return keys.error();
    }
    Expectation expect;
    const auto* states = array_of(field(value, "states"));
    const auto* deliveries = array_of(field(value, "deliveries"));
    const auto* outcomes = array_of(field(value, "audio"));
    if (states == nullptr || deliveries == nullptr || outcomes == nullptr) {
        return malformed_field();
    }
    for (const auto& state : *states) {
        const auto name = string_of(state);
        if (!name) {
            return name.error();
        }
        expect.states.push_back(name.value());
    }
    for (const auto& item : *deliveries) {
        if (const auto keys = expect_keys(item, {"role", "sequence", "ts", "payload"}); !keys) {
            return keys.error();
        }
        DeliveryOutcome delivery;
        const auto role = role_of(field(item, "role"));
        const auto sequence = u64_of(field(item, "sequence"));
        const auto ts = int_of(field(item, "ts"));
        if (!role || !sequence || !ts) {
            return malformed_field();
        }
        delivery.role = role.value();
        delivery.sequence = sequence.value();
        delivery.timestamp = core::Nanoseconds{ts.value()};
        if (const auto blob = blob_of(field(item, "payload"), delivery.payload); !blob) {
            return blob.error();
        }
        expect.deliveries.push_back(std::move(delivery));
    }
    for (const auto& item : *outcomes) {
        if (const auto keys = expect_keys(item, {"role", "rate", "bits", "channels", "ts",
                                                 "pcm"});
            !keys) {
            return keys.error();
        }
        AudioOutcome outcome;
        const auto role = audio_role_of(field(item, "role"));
        const auto rate = u32_of(field(item, "rate"));
        const auto bits = u8_of(field(item, "bits"));
        const auto channels = u8_of(field(item, "channels"));
        const auto ts = int_of(field(item, "ts"));
        if (!role || !rate || !bits || !channels || !ts) {
            return malformed_field();
        }
        outcome.role = role.value();
        outcome.format = audio::AudioFormat{rate.value(), channels.value(), bits.value()};
        outcome.timestamp = core::Nanoseconds{ts.value()};
        if (const auto blob = blob_of(field(item, "pcm"), outcome.pcm); !blob) {
            return blob.error();
        }
        expect.audio.push_back(std::move(outcome));
    }
    const auto sends = int_of(field(value, "sends"));
    if (!sends || sends.value() < 0) {
        return malformed_field();
    }
    expect.sends = sends.value();
    return expect;
}

core::Result<Fixture> parse_body(const JsonValue& body) {
    if (const auto keys = expect_keys(body, {"provenance", "metadata", "services", "records",
                                             "expect"});
        !keys) {
        return keys.error();
    }
    const auto provenance = string_of(field(body, "provenance"));
    const auto metadata = string_of(field(body, "metadata"));
    if (!provenance || !metadata || provenance.value() != kSyntheticProvenance) {
        return malformed_field();
    }
    const auto* services = array_of(field(body, "services"));
    const auto* records = array_of(field(body, "records"));
    if (services == nullptr || records == nullptr) {
        return malformed_field();
    }
    Fixture fixture;
    fixture.metadata = metadata.value();
    for (const auto& item : *services) {
        const auto service = parse_service(item);
        if (!service) {
            return service.error();
        }
        fixture.services.push_back(service.value());
    }
    for (const auto& item : *records) {
        const auto record = parse_record(item);
        if (!record) {
            return record.error();
        }
        fixture.records.push_back(record.value());
    }
    const auto expect = parse_expectation(field(body, "expect"));
    if (!expect) {
        return expect.error();
    }
    fixture.expect = expect.value();
    return fixture;
}

} // namespace detail

core::Result<Fixture> load_fixture(std::span<const std::byte> file_bytes) {
    if (file_bytes.empty() || file_bytes.size() > kMaxFixtureBytes) {
        return Error{ErrorCode::transport_oversize_frame};
    }
    const std::string_view file{reinterpret_cast<const char*>(file_bytes.data()),
                                file_bytes.size()};
    if (!file.starts_with(kFilePrefix) || !file.ends_with("}")) {
        return malformed();
    }
    const std::size_t hash_at = kFilePrefix.size();
    const std::size_t body_at = hash_at + 64 + kFileSeparator.size();
    if (file.size() < body_at + 2) {
        return malformed();
    }
    const std::string_view hash = file.substr(hash_at, 64);
    for (const char c : hash) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return malformed();
        }
    }
    if (file.substr(hash_at + 64, kFileSeparator.size()) != kFileSeparator) {
        return malformed();
    }
    const std::string_view body = file.substr(body_at, file.size() - body_at - 1);
    if (body.empty() || body.front() != '{') {
        return malformed();
    }
    const auto parsed = detail::parse_json(body);
    if (!parsed) {
        return parsed.error();
    }
    const auto fixture = detail::parse_body(parsed.value());
    if (!fixture) {
        return fixture.error();
    }
    if (const auto valid = detail::validate_fixture(fixture.value()); !valid) {
        return valid.error();
    }
    if (detail::encode_canonical_body(fixture.value()) != body) {
        return malformed();
    }
    std::string hash_input(kSchemaName);
    hash_input += "\n";
    hash_input += body;
    const auto hash_span = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(hash_input.data()), hash_input.size()};
    if (detail::sha256_hex(hash_span) != hash) {
        return malformed();
    }
    return fixture.value();
}

core::Result<void> check_fixture(std::span<const std::byte> file_bytes) {
    const auto fixture = load_fixture(file_bytes);
    if (!fixture) {
        return fixture.error();
    }
    return {};
}

} // namespace aa::replay
