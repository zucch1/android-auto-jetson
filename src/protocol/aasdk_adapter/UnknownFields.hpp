// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Extension-tolerance separation (pre-28 conformance, decision
// decision-oaa-aasdk-conformance.json action item 5): classify the protobuf
// unknown-field set of one parsed message into two wire-semantic classes
// BEFORE any decoding expansion.
//
//   1. unknown_enum_or_wiretype  - an entry whose tag IS in the pinned schema.
//      Proto2 could not interpret it: either an enum value the pinned enum
//      lacks (proto2 then silently defaults the field - the hazard documented
//      in MediaConfiguration.cpp) or a wire-type mismatch on a known tag.
//      This class is ALWAYS malformed and must stay rejected: it is a
//      misreading hazard, not an extension.
//
//   2. extension_tag - an entry whose tag is NOT in the pinned schema: a
//      forward-compatibility extension (e.g. AdditionalVideoConfig f5-8 at GAL
//      4.3+, ServiceDiscoveryResponse slots 15-18 at AA 17.x). Proto2 semantics
//      say preserve/ignore such fields. Whether THIS boundary tolerates them
//      is an explicit ExtensionPolicy decision, not an accident of the
//      protobuf runtime.
//
// Policy status (pre-28): the strict profile (ExtensionPolicy::reject) is the
// default everywhere and preserves today's fail-closed behavior byte-for-byte.
// Moving receive paths to ExtensionPolicy::tolerate is PROPOSED only, pending
// phone evidence (Galaxy S24 probe, action item 6) - see the wire-conformance
// matrix discrepancy rows D3/D5/D9/D15 and the conformance report deliverable 3.
//
// HARD BOUNDARY: this header lives under src/**/*_adapter/ so its external
// includes are legal here and nowhere else (tests/architecture/boundary_scan.py).

#include <google/protobuf/message.h>
#include <google/protobuf/unknown_field_set.h>

#include <span>
#include <vector>

namespace aa::protocol::aasdk_adapter {

enum class UnknownFieldOrigin {
    unknown_enum_or_wiretype, // known tag, uninterpretable value (class 1)
    extension_tag,            // unseen tag (class 2)
};

struct UnknownFieldIssue final {
    UnknownFieldOrigin origin{};
    int tag{};

    [[nodiscard]] friend constexpr bool operator==(const UnknownFieldIssue&,
                                                   const UnknownFieldIssue&) noexcept = default;
};

// Strict (today's behavior): every unknown entry is rejected. Tolerate: only
// class 1 is rejected; class 2 extensions are ignored (proposed, not enabled).
enum class ExtensionPolicy { reject, tolerate };

struct UnknownFieldVerdict final {
    std::vector<UnknownFieldIssue> issues{};

    [[nodiscard]] bool empty() const noexcept { return issues.empty(); }
    [[nodiscard]] bool has_unknown_enum_or_wiretype() const noexcept {
        for (const auto& issue : issues) {
            if (issue.origin == UnknownFieldOrigin::unknown_enum_or_wiretype) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] bool extension_only() const noexcept {
        return !issues.empty() && !has_unknown_enum_or_wiretype();
    }

    // The malformed class is never tolerable.
    [[nodiscard]] bool malformed() const noexcept { return has_unknown_enum_or_wiretype(); }

    // Combined accept/reject decision under one explicit policy.
    [[nodiscard]] bool rejected_under(ExtensionPolicy policy) const noexcept {
        if (malformed()) {
            return true;
        }
        return !issues.empty() && policy == ExtensionPolicy::reject;
    }
};

// Classify one message's unknown fields against its pinned-schema tags.
// `known_tags` must be the full set of field tags the pinned schema defines
// for this message type; entries with any other tag are extensions.
[[nodiscard]] inline UnknownFieldVerdict
classify_unknown_fields(const google::protobuf::Message& message,
                        std::span<const int> known_tags) {
    UnknownFieldVerdict verdict;
    const auto& unknown = message.GetReflection()->GetUnknownFields(message);
    const int count = unknown.field_count();
    verdict.issues.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        const int tag = unknown.field(i).number();
        bool known = false;
        for (const int candidate : known_tags) {
            if (candidate == tag) {
                known = true;
                break;
            }
        }
        verdict.issues.push_back(UnknownFieldIssue{
            known ? UnknownFieldOrigin::unknown_enum_or_wiretype
                  : UnknownFieldOrigin::extension_tag,
            tag});
    }
    return verdict;
}

// The boundary default until phone evidence justifies tolerance.
inline constexpr ExtensionPolicy kStrictExtensionPolicy = ExtensionPolicy::reject;

} // namespace aa::protocol::aasdk_adapter
