// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include "../ipc/dbus_adapter/BusConnection.hpp"
#include <mutex>

namespace aa::helper {
using namespace ipc::dbus_adapter;
using Clock = std::chrono::steady_clock;
struct BusClient::Impl {
    std::timed_mutex mutex;
    BusConnection bus;
    trust::ApprovedPhoneStore& store;
    struct Pending { Source source; Action action; std::uint64_t grant; };
    std::vector<Pending> pending;
    std::optional<Source> active_source;
    std::optional<Source> ap_source;
    std::string helper_owner;
    Impl(BusConnection connection, trust::ApprovedPhoneStore& approvals)
        : bus(std::move(connection)), store(approvals) {}
    // All transaction helpers require mutex ownership, including failure cleanup.
    core::Result<std::uint64_t> rpc(const char* member, const char* signature, MessageWriter writer,
                                    Clock::time_point until, ObservedPeer* observed = nullptr,
                                    HandoffFailure* failure = nullptr, ApSnapshot* snapshot = nullptr) {
        if (Clock::now() >= until) return Error{ErrorCode::timeout};
        bus.deadline(until);
        const auto owner = bus.get_name_owner(ipc::BusName{kBusName});
        if (!owner) return Error{Clock::now() >= until ? ErrorCode::timeout : ErrorCode::helper_unavailable};
        if (!helper_owner.empty() && helper_owner != owner.value().value) {
            pending.clear(); active_source.reset(); ap_source.reset(); helper_owner = owner.value().value;
            return Error{ErrorCode::helper_rejected};
        }
        helper_owner = owner.value().value;
        writer.destination(owner.value().value).path(kObjectPath).interface_name(kInterface)
              .member(member).body_signature(signature);
        const auto serial = bus.send_message(MessageType::method_call, writer);
        if (!serial) return Error{Clock::now() >= until ? ErrorCode::timeout : ErrorCode::helper_unavailable};
        auto reply = bus.await_reply(serial.value(), owner.value(), snapshot != nullptr);
        if (!reply) return Error{Clock::now() >= until ? ErrorCode::timeout : ErrorCode::helper_unavailable};
        const core::SecretScope wipe_body{reply.value().body, snapshot != nullptr};
        if (Clock::now() >= until) return Error{ErrorCode::timeout};
        if (reply.value().type != MessageType::method_return || reply.value().signature !=
            (snapshot ? "utssssut" : failure ? "utssu" : observed ? "utss" : "ut"))
            return Error{ErrorCode::helper_protocol_mismatch};
        Cursor body{reply.value().body};
        const auto error = body.read_u32();
        const auto value = body.read_u64();
        if (!error || !value) return Error{ErrorCode::helper_protocol_mismatch};
        if (observed) {
            const auto address = body.read_string();
            const auto adapter = body.read_string();
            if (!address || !adapter || address.value().size() > 64 || adapter.value().size() > 64)
                return Error{ErrorCode::helper_protocol_mismatch};
            *observed = ObservedPeer{value.value(), {address.value(), adapter.value()}};
        }
        if (failure) {
            const auto code = body.read_u32();
            if (!code || code.value() > static_cast<std::uint32_t>(HandoffFailure::rebind)) return Error{ErrorCode::helper_protocol_mismatch};
            *failure = static_cast<HandoffFailure>(code.value());
        }
        if (snapshot) {
            const auto ssid = body.read_string_view(); const auto password = body.read_string_view();
            const auto bssid = body.read_string_view(); const auto ip = body.read_string_view();
            const auto port = body.read_u32(); const auto generation = body.read_u64();
            if (!ssid || !password || !bssid || !ip || !port || !generation || ssid.value().size() > 32 ||
                password.value().size() > 64 || bssid.value().size() > 17 || ip.value().size() > 15)
                return Error{ErrorCode::helper_protocol_mismatch};
            if (error.value() == 0) {
                if (ssid.value().empty() || password.value().size() < 8 || bssid.value().size() != 17 ||
                    ip.value().empty() || port.value() < 1024 || port.value() > 65535 || !generation.value())
                    return Error{ErrorCode::helper_protocol_mismatch};
                *snapshot = ApSnapshot{{ssid.value(), password.value(), bssid.value(), ip.value(), port.value(), generation.value()}};
            } else if (!ssid.value().empty() || !password.value().empty() || !bssid.value().empty() ||
                       !ip.value().empty() || port.value() || generation.value()) return Error{ErrorCode::helper_protocol_mismatch};
        }
        if (!body.done()) return Error{ErrorCode::helper_protocol_mismatch};
        switch (error.value()) {
        case 0: return value.value();
        case 1000: return Error{ErrorCode::helper_unavailable};
        case 1001: return Error{ErrorCode::helper_rejected};
        default: return Error{ErrorCode::helper_protocol_mismatch};
        }
    }
    core::Result<State> execute(Invocation invocation, Clock::time_point until) {
        const auto [action, channel, grant] = invocation;
        MessageWriter writer;
        writer.put_u32(static_cast<std::uint32_t>(action)).put_u32(channel).put_u64(grant);
        const auto result = rpc("Execute", "uut", writer, until);
        if (!result) return result.error();
        if (result.value() > static_cast<std::uint64_t>(State::unavailable))
            return Error{ErrorCode::helper_protocol_mismatch};
        return static_cast<State>(result.value());
    }
    core::Result<void> invalidate_locked(Source source, Clock::time_point until) {
        if (ap_source && (!source.generation || source.generation == ap_source->generation) &&
            (!source.confirmation || source.confirmation == ap_source->confirmation)) ap_source.reset();
        if (active_source && (!source.generation || source.generation == active_source->generation) &&
            (!source.confirmation || source.confirmation == active_source->confirmation)) active_source.reset();
        std::erase_if(pending, [source](const Pending& p) {
            return (!source.generation || p.source.generation == source.generation) &&
                   (!source.confirmation || p.source.confirmation == source.confirmation);
        });
        MessageWriter writer;
        writer.put_u64(source.generation).put_u64(source.confirmation);
        const auto result = rpc("Invalidate", "tt", writer, until);
        return result ? core::Result<void>{} : core::Result<void>{result.error()};
    }
    void invalidate(Source source) {
        const std::lock_guard lock{mutex};
        (void)invalidate_locked(source, Clock::now() + std::chrono::milliseconds(500));
    }
    core::Result<ObservedPeer> observe(Clock::time_point until) {
        ObservedPeer peer;
        const auto polled = rpc("PollPeerV1", "", {}, until, &peer);
        if (!polled) return polled.error();
        if (!peer.handle) return Error{ErrorCode::helper_rejected};
        return peer;
    }
    core::Result<void> revoke(const PeerIdentity& identity, Clock::time_point until) {
        MessageWriter writer;
        writer.put_string(identity.address).put_string(identity.adapter);
        const auto result = rpc("RevokePeerV1", "ss", writer, until);
        return result ? core::Result<void>{} : core::Result<void>{result.error()};
    }
};
}
