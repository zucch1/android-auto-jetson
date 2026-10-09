// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/replay/Schema.hpp>

#include "FixtureValidate.hpp"
#include "Sha256.hpp"

#include <string>

namespace aa::replay {

core::Result<ExportedFixture> export_fixture(const Fixture& fixture) {
    if (const auto valid = detail::validate_fixture(fixture); !valid) {
        return valid.error();
    }
    const std::string body = detail::encode_canonical_body(fixture);
    std::string hash_input(kSchemaName);
    hash_input += "\n";
    hash_input += body;
    const auto hash_span = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(hash_input.data()), hash_input.size()};
    ExportedFixture exported;
    exported.sha256_hex = detail::sha256_hex(hash_span);
    std::string file = "{\"schema\":\"";
    file += kSchemaName;
    file += "\",\"sha256\":\"";
    file += exported.sha256_hex;
    file += "\",\"body\":";
    file += body;
    file += "}";
    if (file.size() > kMaxFixtureBytes) {
        return Error{ErrorCode::transport_oversize_frame};
    }
    exported.file_bytes.resize(file.size());
    for (std::size_t i = 0; i < file.size(); ++i) {
        exported.file_bytes[i] = static_cast<std::byte>(file[i]);
    }
    return exported;
}

} // namespace aa::replay
