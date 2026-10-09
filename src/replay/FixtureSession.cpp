// SPDX-License-Identifier: GPL-3.0-or-later
#include "FixtureParse.hpp"

#include "FieldAccess.hpp"

#include <aa/replay/Schema.hpp>
#include <aa/session/Events.hpp>

namespace aa::replay::detail {
namespace {

constexpr Error malformed() { return Error{ErrorCode::invalid_argument}; }

} // namespace

core::Result<void> parse_session_body(const JsonValue& value, SessionRecord& step) {
    const auto op_text = string_of(field(value, "op"));
    if (!op_text) {
        return op_text.error();
    }
    const auto op = parse_session_op(op_text.value());
    if (!op) {
        return op.error();
    }
    step.op = op.value();
    switch (step.op) {
    case SessionOp::event: {
        if (const auto keys = expect_keys(value, {"kind", "t", "op", "event"}); !keys) {
            return keys.error();
        }
        const auto name = string_of(field(value, "event"));
        if (!name) {
            return name.error();
        }
        for (const session::Event event :
             {session::Event::start_discovery, session::Event::transport_ready,
              session::Event::negotiation_succeeded, session::Event::degrade,
              session::Event::recover, session::Event::link_lost, session::Event::reconnect,
              session::Event::cleanup_finished, session::Event::reset}) {
            if (session::to_string(event) == name.value()) {
                step.event = event;
                return {};
            }
        }
        return malformed();
    }
    case SessionOp::phone_discovered:
    case SessionOp::authorize_pairing:
    case SessionOp::confirm_pairing: {
        if (const auto keys = expect_keys(value, {"kind", "t", "op", "phone"}); !keys) {
            return keys.error();
        }
        const auto phone = string_of(field(value, "phone"));
        if (!phone) {
            return phone.error();
        }
        step.phone_key = phone.value();
        return {};
    }
    case SessionOp::fail: {
        if (const auto keys = expect_keys(value, {"kind", "t", "op", "code"}); !keys) {
            return keys.error();
        }
        const auto code = u16_of(field(value, "code"));
        if (!code) {
            return code.error();
        }
        step.fail_code = static_cast<ErrorCode>(code.value());
        return {};
    }
    case SessionOp::request_stop:
    case SessionOp::tick:
    case SessionOp::attach_fresh:
    case SessionOp::reset_channels: {
        return expect_keys(value, {"kind", "t", "op"});
    }
    }
    return malformed();
}

} // namespace aa::replay::detail
