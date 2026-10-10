// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Fixture body parsers: strict positional schema mapping from the bounded
// JSON tree into typed fixture values. Split across FixtureServices/
// FixtureSession/FixtureRecords/FixtureDecode so each unit stays reviewable.

#include <aa/core/Result.hpp>
#include <aa/replay/Fixture.hpp>

#include "Json.hpp"

namespace aa::replay::detail {

[[nodiscard]] core::Result<ServiceEntry> parse_service(const JsonValue& value);
[[nodiscard]] core::Result<void> parse_session_body(const JsonValue& value, SessionRecord& step);
[[nodiscard]] core::Result<Record> parse_record(const JsonValue& value);
[[nodiscard]] core::Result<Expectation> parse_expectation(const JsonValue& value);
[[nodiscard]] core::Result<Fixture> parse_body(const JsonValue& body);

} // namespace aa::replay::detail
