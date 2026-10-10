// SPDX-License-Identifier: GPL-3.0-or-later
// Pre-28 extension-tolerance separation tests (decision action item 5):
// unknown fields are classified into (1) unknown-enum/wire-type on a known tag
// [always malformed - proto2 silent-default hazard] and (2) unseen extension
// tags [policy], and the project-owned decode path keeps failing closed under
// the strict profile. The classification itself is the separation; flipping
// the extension policy is PROPOSED only (conformance report deliverable 3).

#include "../replay/conformance_wire_spec.hpp"

#include <aap_protobuf/service/Service.pb.h>
#include <aap_protobuf/service/control/message/ServiceDiscoveryResponse.pb.h>

#include "../../src/protocol/aasdk_adapter/UnknownFields.hpp"

#include <aa/protocol/Messages.hpp>

#include <array>
#include <vector>

#include <gtest/gtest.h>

namespace {
namespace proto = aa::protocol;
namespace adapter = aa::protocol::aasdk_adapter;
using aap_protobuf::service::Service;
using adapter::ExtensionPolicy;
using adapter::UnknownFieldOrigin;

std::vector<std::byte> bytes_of(std::initializer_list<int> values) {
    std::vector<std::byte> out;
    out.reserve(values.size());
    for (const int value : values) {
        out.push_back(static_cast<std::byte>(value & 0xFF));
    }
    return out;
}

std::vector<std::byte> sdr_of(const std::vector<std::byte>& service_body) {
    std::vector<std::byte> out = bytes_of({0x00, 0x06, 0x0A,
                                           static_cast<int>(service_body.size())});
    out.insert(out.end(), service_body.begin(), service_body.end());
    return out;
}

constexpr int kServiceTags[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};

Service parse_service(const std::vector<std::byte>& service_body) {
    Service service;
    EXPECT_TRUE(service.ParseFromArray(service_body.data(),
                                       static_cast<int>(service_body.size())));
    return service;
}

} // namespace

TEST(ConformanceTolerance, CleanEntryCarriesNoUnknownFields) {
    // Given: the spec-derived clean video service body (F-E inner Service).
    const auto service = parse_service(
        bytes_of({0x08, 0x63, 0x1A, 0x0A, 0x08, 0x03, 0x22, 0x06, 0x08, 0x02, 0x10, 0x02, 0x50, 0x03}));
    // When: the boundary classifies its unknown fields.
    const auto verdict = adapter::classify_unknown_fields(service, kServiceTags);
    // Then: nothing to classify and nothing is rejected under any policy.
    EXPECT_TRUE(verdict.empty());
    EXPECT_FALSE(verdict.rejected_under(ExtensionPolicy::reject));
    EXPECT_FALSE(verdict.rejected_under(ExtensionPolicy::tolerate));
}

TEST(ConformanceTolerance, UnseenTagIsClassifiedAsExtensionNotMalformed) {
    // Given: a well-formed Service entry carrying an unseen tag 15 (varint 5)
    // exactly as an AA 17.x descriptor extension would appear on the wire.
    const auto service = parse_service(bytes_of({0x08, 0x01, 0x78, 0x05}));
    // When: the boundary classifies it.
    const auto verdict = adapter::classify_unknown_fields(service, kServiceTags);
    // Then: it is the extension class, never the malformed class.
    ASSERT_EQ(verdict.issues.size(), 1u);
    EXPECT_EQ(verdict.issues[0].origin, UnknownFieldOrigin::extension_tag);
    EXPECT_EQ(verdict.issues[0].tag, 15);
    EXPECT_TRUE(verdict.extension_only());
    EXPECT_FALSE(verdict.malformed());
    // And: the strict profile (today's behavior) still rejects it...
    EXPECT_TRUE(verdict.rejected_under(ExtensionPolicy::reject));
    // ...while the proposed tolerant profile would not.
    EXPECT_FALSE(verdict.rejected_under(ExtensionPolicy::tolerate));
}

TEST(ConformanceTolerance, UnknownEnumOnKnownTagIsMalformedUnderEveryPolicy) {
    // Given: MediaSinkService.available_type = 99 - a codec value the pinned
    // enum lacks. Proto2 stores it as an unknown field and would silently
    // default available_type() to MEDIA_CODEC_AUDIO_PCM.
    const auto service = parse_service(bytes_of({0x08, 0x01, 0x1A, 0x02, 0x08, 0x63}));
    ASSERT_TRUE(service.has_media_sink_service());
    // Then: the classification is the malformed class at the field's own tag.
    const auto verdict =
        adapter::classify_unknown_fields(service.media_sink_service(),
                                         std::array<int, 8>{1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_EQ(verdict.issues.size(), 1u);
    EXPECT_EQ(verdict.issues[0].origin, UnknownFieldOrigin::unknown_enum_or_wiretype);
    EXPECT_EQ(verdict.issues[0].tag, 1);
    EXPECT_TRUE(verdict.malformed());
    EXPECT_FALSE(verdict.extension_only());
    // And: it is rejected even under the tolerant profile - the silent-default
    // hazard is never an extension.
    EXPECT_TRUE(verdict.rejected_under(ExtensionPolicy::tolerate));
    // And: the proto2 silent default is real (this is WHY the class exists).
    EXPECT_EQ(service.media_sink_service().available_type(),
              aap_protobuf::service::media::shared::message::MEDIA_CODEC_AUDIO_PCM);
}

TEST(ConformanceTolerance, WireTypeMismatchOnKnownTagIsMalformed) {
    // Given: MediaSinkService.available_type (tag 1, varint) sent as
    // length-delimited - the optional-field flavor of a wire-type mismatch.
    const auto service = parse_service(bytes_of({0x08, 0x01, 0x1A, 0x03, 0x0A, 0x01, 0x00}));
    ASSERT_TRUE(service.has_media_sink_service());
    EXPECT_FALSE(service.media_sink_service().has_available_type());
    // Then: the entry cannot be interpreted and classifies as malformed.
    const auto verdict =
        adapter::classify_unknown_fields(service.media_sink_service(),
                                         std::array<int, 8>{1, 2, 3, 4, 5, 6, 7, 8});
    ASSERT_EQ(verdict.issues.size(), 1u);
    EXPECT_EQ(verdict.issues[0].origin, UnknownFieldOrigin::unknown_enum_or_wiretype);
    EXPECT_TRUE(verdict.rejected_under(ExtensionPolicy::tolerate));
    // And: the required-field flavor is rejected even earlier, at parse time
    // (Service.id tag 1 as length-delimited leaves required id missing).
    Service required_mismatch;
    EXPECT_FALSE(required_mismatch.ParseFromArray(
        std::array<int, 3>{0x0A, 0x01, 0x00}.data(), 3));
}

TEST(ConformanceTolerance, DecodePathRemainsFailClosedOnBothClasses) {
    // Given: discovery responses wrapping an extension-tag entry and an
    // unknown-enum entry.
    const auto with_extension = sdr_of(bytes_of({0x08, 0x01, 0x78, 0x05}));
    const auto with_unknown_enum = sdr_of(bytes_of({0x08, 0x01, 0x1A, 0x02, 0x08, 0x63}));
    // Then: the project-owned decode path rejects both under the strict
    // profile (behavior preserved from before the separation).
    const auto ext = proto::decode_service_discovery_response(with_extension);
    ASSERT_FALSE(ext.has_value());
    EXPECT_EQ(ext.error().code(), aa::ErrorCode::protocol_malformed_message);
    const auto enum_gap = proto::decode_service_discovery_response(with_unknown_enum);
    ASSERT_FALSE(enum_gap.has_value());
    EXPECT_EQ(enum_gap.error().code(), aa::ErrorCode::protocol_malformed_message);
}

TEST(ConformanceTolerance, DecodePathStillAcceptsCleanSpecBytes) {
    // Given: the hand-derived spec response (F-E).
    // Then: the separation did not change acceptance of clean input.
    const auto decoded = proto::decode_service_discovery_response(
        conformancewire::F_E_service_discovery_response_payload);
    EXPECT_TRUE(decoded.has_value());
}
