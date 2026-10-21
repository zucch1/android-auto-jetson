// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <aa/ipc/ControlBusService.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include <fstream>
#include <future>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace aa;
using namespace aa::helper;
class RecordingBackend final : public Backend {
public:
    explicit RecordingBackend(const char* path) : path_(path) {}
    core::Result<State> apply(Action action, std::uint32_t) override {
        if (action == Action::release_ap && std::getenv("AA_HELPER_HOLD_RELEASE")) {
            std::cout << "entered\n" << std::flush;
            ::pollfd input{STDIN_FILENO, POLLIN, 0};
            std::string release;
            if (::poll(&input, 1, 5000) <= 0 || !std::getline(std::cin, release) || release != "release")
                return Error{ErrorCode::helper_unavailable};
        }
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
    struct Worker {
        core::CancellationSource cancellation;
        std::future<unsigned int> result;
        std::jthread thread;
    };
    Worker first, second;
    std::cout << "ready\n" << std::flush;
    for (;;) {
        if (!service.value().cycle(core::Milliseconds{10})) return 5;
        ::pollfd input{STDIN_FILENO, POLLIN, 0};
        if (::poll(&input, 1, 0) <= 0) continue;
        std::string command;
        if (!std::getline(std::cin, command)) return 0;
        core::Result<void> result{Error{ErrorCode::invalid_argument}};
        if (command.starts_with("worker-")) {
            auto& worker = command == "worker-release" ? first : second;
            if (worker.thread.joinable()) return 6;
            worker.cancellation = core::CancellationSource{};
            std::promise<unsigned int> response;
            worker.result = response.get_future();
            std::promise<pid_t> started;
            auto tid = started.get_future();
            worker.thread = std::jthread{[&, command, response = std::move(response), started = std::move(started)]() mutable {
                started.set_value(::gettid());
                if (command == "worker-status") {
                    const auto reply = helper.value()->status(Action::ap_status);
                    response.set_value(reply ? 0U : static_cast<unsigned int>(reply.error().code()));
                } else {
                    const Request request{command == "worker-release" ? Operation::teardown_access_point :
                                          Operation::ensure_access_point, 36};
                    const auto reply = helper.value()->call(request, core::Milliseconds{command == "worker-timeout" ? 100 : 1000},
                                                           worker.cancellation.get_token());
                    response.set_value(reply ? 0U : static_cast<unsigned int>(reply.error().code()));
                }
            }};
            std::cout << ::getpid() << ' ' << tid.get() << '\n' << std::flush;
            continue;
        } else if (command == "stop-worker") {
            second.cancellation.request_stop();
            result = {};
        } else if (command == "join-first" || command == "join-second") {
            auto& worker = command == "join-first" ? first : second;
            if (worker.result.wait_for(std::chrono::seconds(2)) != std::future_status::ready) return 7;
            std::cout << worker.result.get() << '\n' << std::flush;
            worker.thread.join();
            continue;
        } else if (command == "duplicate-confirm") {
            if (!helper.value()->confirmed(core::ConsumerId{777}, core::PairingRequestId{888})) return 8;
            result = helper.value()->confirmed(core::ConsumerId{777}, core::PairingRequestId{888});
        } else if (command == "acquire" || command == "pair" || command == "release" || command == "wrong-source" ||
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
