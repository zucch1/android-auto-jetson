// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/core/Result.hpp>
#include <cstdint>
#include <chrono>
#include <string>

namespace aa::helper {
struct ReceiverIdentity final {
    std::string unique_name;
    std::uint32_t uid{};
    std::uint32_t pid{};
    std::uint64_t starttime{};
    std::string digest;
    friend bool operator==(const ReceiverIdentity&, const ReceiverIdentity&) = default;
};
class IdentityLookup {
public:
    virtual ~IdentityLookup() = default;
    virtual core::Result<ReceiverIdentity> resolve(const std::string& unique_name) = 0;
};
struct IdentityPolicy final {
    std::uint32_t session_uid{};
    std::string digest;
    // Production always loads root-owned, non-writable absolute paths.
    static core::Result<IdentityPolicy> load(const std::string& uid_file,
                                              const std::string& digest_file,
                                              std::uint32_t owner = 0);
};
core::Result<ReceiverIdentity> inspect_process(const std::string& unique_name,
                                             std::uint32_t uid, std::uint32_t pid,
                                             const IdentityPolicy& policy,
                                             std::chrono::steady_clock::time_point deadline =
                                                 std::chrono::steady_clock::now() + std::chrono::milliseconds(500));
} // namespace aa::helper
