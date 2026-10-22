// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusService.hpp>
#include "../../src/ipc/dbus_adapter/BusConnection.hpp"
#include <iostream>
#include <sstream>

int main(int argc, char** argv) {
    using namespace aa;
    using namespace ipc::dbus_adapter;
    if (argc != 2) return 2;
    auto bus = BusConnection::connect_private(argv[1]);
    if (!bus) return 3;
    std::cout << "ready\n" << std::flush;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input{line};
        std::string op, member, signature;
        input >> op;
        MessageWriter writer;
        std::uint64_t value{}, source{};
        if (op == "execute") {
            std::uint32_t action{};
            input >> action; member = "Execute"; signature = "uut";
            writer.put_u32(action).put_u32(0).put_u64(0);
        } else if (op == "poll") member = "PollPeerV1";
        else if (op == "bind") {
            input >> value >> source; member = "BindPeerV1"; signature = "ttt";
            writer.put_u64(1).put_u64(source).put_u64(value);
        } else if (op == "issue") {
            input >> source; member = "Issue"; signature = "ttuu";
            writer.put_u64(1).put_u64(source).put_u32(2).put_u32(0);
        } else if (op == "open") {
            input >> value; member = "Execute"; signature = "uut";
            writer.put_u32(2).put_u32(0).put_u64(value);
        } else if (op == "reconnect") {
            input >> value; member = "ReconnectPeerV1"; signature = "t"; writer.put_u64(value);
        } else if (op == "close") {
            member = "Invalidate"; signature = "tt"; writer.put_u64(0).put_u64(0);
        } else if (op == "targetless") {
            member = "Execute"; signature = "uut"; writer.put_u32(9).put_u32(0).put_u64(0);
        } else if (op == "malformed") {
            member = "BindPeerV1"; signature = "s"; writer.put_string("/org/bluez/hci0/arbitrary");
        } else return 4;
        bus.value().deadline(std::chrono::steady_clock::now() + std::chrono::seconds(2));
        const auto owner = bus.value().get_name_owner(ipc::BusName{helper::kBusName});
        if (!owner) return 5;
        writer.destination(owner.value().value).path(helper::kObjectPath).interface_name(helper::kInterface)
              .member(member).body_signature(signature);
        const auto sent = bus.value().send_message(MessageType::method_call, writer);
        if (!sent) return 6;
        const auto reply = bus.value().await_reply(sent.value(), owner.value());
        if (!reply) return 7;
        Cursor body{reply.value().body};
        const auto error = body.read_u32(); const auto result = body.read_u64();
        if (!error || !result) return 8;
        std::cout << error.value() << ' ' << result.value() << '\n' << std::flush;
    }
}
