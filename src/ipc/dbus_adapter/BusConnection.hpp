// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Minimal real D-Bus client connection (task 21): EXTERNAL auth + Hello and the
// four org.freedesktop.DBus calls the peer-credential lookup needs
// (GetNameOwner, GetConnectionUnixUser, RequestName, ReleaseName). Private to
// src/ipc/dbus_adapter except through BusPeerLookup. Pure POSIX sockets over
// the session bus — no external D-Bus library. All replies are parsed strictly
// by BusWire and malformed answers fail closed with a typed transport error.

#include <aa/core/Result.hpp>
#include <aa/ipc/ControlDispatcher.hpp>
#include <aa/ipc/PeerCredentials.hpp>

#include "BusWire.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace aa::ipc::dbus_adapter {

class BusConnection final {
public:
    ~BusConnection();
    BusConnection(const BusConnection&) = delete;
    BusConnection& operator=(const BusConnection&) = delete;
    BusConnection(BusConnection&& other) noexcept;
    BusConnection& operator=(BusConnection&& other) noexcept;

    // Connects to the session bus named by DBUS_SESSION_BUS_ADDRESS
    // (unix:path=... or unix:abstract=...) and completes AUTH EXTERNAL + Hello.
    [[nodiscard]] static core::Result<BusConnection> connect_session();

    [[nodiscard]] const BusName& unique_name() const noexcept { return unique_name_; }

    // org.freedesktop.DBus.RequestName (no flags); fails when not owned.
    [[nodiscard]] core::Result<void> request_name(const BusName& name);
    [[nodiscard]] core::Result<void> release_name(const BusName& name);
    [[nodiscard]] core::Result<BusName> get_name_owner(const BusName& name);
    [[nodiscard]] core::Result<std::uint32_t> get_connection_unix_uid(const BusName& name);

    // Reads the next message off the bus (bounded strict framing).
    [[nodiscard]] core::Result<ParsedMessage> read_message();

    // Fire-and-forget send of one already-assembled message (signal emission
    // and test reply injection). The serial is allocated from this
    // connection's counter and returned so replies can be matched.
    [[nodiscard]] core::Result<std::uint32_t> send_message(MessageType type,
                                                           const MessageWriter& message);

    // Waits for the method return/error answering `serial`, accepting ONLY
    // messages whose daemon-stamped sender equals `expected_sender` and whose
    // destination is this connection. Forged replies (matching serial, wrong
    // sender/destination) are never accepted and are counted; an exhaustion
    // or timeout after forged candidates is a typed transport_unauthorized_peer.
    [[nodiscard]] core::Result<ParsedMessage> await_reply(std::uint32_t serial,
                                                          const BusName& expected_sender);

    // Sends a dispatch-boundary reply (typed body) addressed to `destination`.
    [[nodiscard]] core::Result<void> send_reply(std::uint32_t reply_serial,
                                                const BusName& destination,
                                                const OutboundReply& reply);

private:
    BusConnection() = default;

    // Sends one method call to the bus daemon and returns its reply.
    [[nodiscard]] core::Result<ParsedMessage> call(std::string_view member,
                                                   std::string_view body_signature,
                                                   MessageWriter body);
    [[nodiscard]] core::Result<void> authenticate();
    [[nodiscard]] core::Result<std::string> read_line();

    int fd_{-1};
    std::uint32_t next_serial_{1};
    BusName unique_name_{};
};

// Maps a daemon-delivered message to the dispatch boundary input. The caller
// identity comes EXCLUSIVELY from the daemon-stamped sender field.
[[nodiscard]] aa::ipc::IncomingCall to_incoming_call(const ParsedMessage& message);

} // namespace aa::ipc::dbus_adapter
