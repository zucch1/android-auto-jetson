// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <unistd.h>

namespace {
using namespace aa;
using namespace aa::helper;
class RecordingBackend final : public Backend {
public:
    explicit RecordingBackend(const char* path) : path_(path) {}
    core::Result<State> apply(Action action, std::uint32_t) override {
        std::ofstream{path_, std::ios::app} << "effect " << static_cast<std::uint32_t>(action) << '\n';
        if (action == Action::release_ap) active_ = false;
        if (action == Action::acquire_ap) active_ = true;
        return active_ ? State::active : State::inactive;
    }
    void cleanup() noexcept override {
        std::ofstream{path_, std::ios::app} << "cleanup\n";
        active_ = false;
    }
private:
    const char* path_;
    bool active_{};
};
int receiver(const char* address, const std::filesystem::path& store_path) {
    trust::ApprovedPhoneStore store{store_path};
    if (!store.load()) return 2;
    trust::SteadyClock clock;
    trust::PairingPolicy phones{store, clock};
    const trust::TransportIdentity identity{trust::WirelessIdentity{"fixture-bond", "fixture-adapter"}};
    phones.note_candidate(identity);
    ipc::RandomPairingIdSource ids;
    auto helper = BusClient::private_bus(address, store);
    if (!helper) return 3;
    auto service = ipc::ControlBusService::start({::getuid()}, {ids, phones, helper.value().get()});
    if (!service) return 4;
    std::cout << "ready\n" << std::flush;
    for (;;) {
        if (!service.value().cycle(core::Milliseconds{10})) return 5;
        ::pollfd input{STDIN_FILENO, POLLIN, 0};
        if (::poll(&input, 1, 0) <= 0) continue;
        std::string command;
        if (!std::getline(std::cin, command)) return 0;
        core::Result<void> result{Error{ErrorCode::invalid_argument}};
        if (command == "acquire" || command == "pair" || command == "release" || command == "wrong-source" ||
            command == "cancelled" || command == "timeout") {
            const auto op = command == "pair" ? Operation::open_pairing_window :
                            command == "release" ? Operation::teardown_access_point : Operation::ensure_access_point;
            Request request{op, 36};
            if (command == "wrong-source") {
                request.consumer_generation = core::ConsumerId{999};
                request.confirmation_source = core::PairingRequestId{999};
            }
            core::CancellationSource cancellation;
            if (command == "cancelled") cancellation.request_stop();
            const auto response = helper.value()->call(request, core::Milliseconds{command == "timeout" ? 0 : 1000}, cancellation.get_token());
            result = response ? core::Result<void>{} : core::Result<void>{response.error()};
        } else if (command == "reconnect" || command == "unknown") {
            auto candidate = identity;
            if (command == "unknown") candidate = trust::WirelessIdentity{"different", "fixture-adapter"};
            const auto response = helper.value()->reconnect(candidate);
            result = response ? core::Result<void>{} : core::Result<void>{response.error()};
        } else if (command == "status" || command == "usb-status" || command == "modem-status") {
            const auto action = command == "usb-status" ? Action::usb_status :
                                command == "modem-status" ? Action::modem_status : Action::ap_status;
            const auto response = helper.value()->status(action);
            result = response ? core::Result<void>{} : core::Result<void>{response.error()};
        } else if (command == "phone") {
            std::cout << "phone " << (store.phones().empty() ? 0 : store.phones().front().phone_id.value) << '\n' << std::flush;
            continue;
        } else if (command == "refresh") {
            service.value().service().refresh_consumer_owner();
            result = {};
        } else if (command == "quit") return 0;
        std::cout << (result ? 0 : static_cast<unsigned int>(result.error().code())) << '\n' << std::flush;
    }
}
}
int main(int argc, char** argv) {
    if (argc != 5) return 2;
    if (std::string{argv[1]} == "receiver") return receiver(argv[2], argv[3]);
    if (std::string{argv[1]} != "helper") return 2;
    auto policy = aa::helper::IdentityPolicy::load(std::string{argv[3]} + "/session.uid",
                                                 std::string{argv[3]} + "/receiver.sha256", ::getuid());
    if (!policy) return 3;
    RecordingBackend backend{argv[4]};
    auto service = aa::helper::BusService::private_bus(argv[2], policy.value(), backend);
    if (!service) return 4;
    std::cout << "ready\n" << std::flush;
    for (;;) if (!service.value().cycle()) return 5;
}
