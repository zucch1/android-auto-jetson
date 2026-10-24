// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "host_endpoint.hpp"
#define AA_NM_TEST_ENDPOINT wireless_test::host_endpoint().c_str()
// NOLINTNEXTLINE(bugprone-suspicious-include): reuse the private-bus composed process fixture.
#include "../helper/nm_test.cpp"
#include "fixture.hpp"
#include <sstream>

namespace aa::helper::nm {
namespace {
struct SecretDiagnostics final : diagnostics::DiagnosticSink {
    std::string recorded;
    void emit_json(std::string_view json) override { recorded.append(json); recorded += '\n'; }
};
class SecretDaemon : public NmDaemon {
protected:
    static constexpr std::string_view password =
        "f17a37c0de5ec0de0123456789abcdef00112233445566778899aabbccddeeff";
    std::string sentinel;
    void SetUp() override {
        NmDaemon::SetUp();
        ASSERT_FALSE(HasFatalFailure());
        sentinel = std::string{password};
        ASSERT_EQ(sentinel.size(), 64U);
        save(directory / "state/ap.credentials", "AA-0123456789abcdef\n" + sentinel + "\n");
        struct stat metadata{};
        ASSERT_EQ(::stat((directory / "state/ap.credentials").c_str(), &metadata), 0);
        ASSERT_EQ(metadata.st_size, 85); ASSERT_EQ(metadata.st_mode & 0777, 0600U);
    }
    void start() {
        const std::vector<std::string> args{std::string{AA_HELPER_BUILD} + "/aa-system-helper",
            "--private-bus", address, directory.string(), "--combined", (directory / "commands").string()};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        ASSERT_EQ(::posix_spawn_file_actions_init(&actions), 0);
        ASSERT_EQ(::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
            (directory / "helper-output").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600), 0);
        ASSERT_EQ(::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO), 0);
        const auto spawned = ::posix_spawn(&daemon.pid, argv[0], &actions, nullptr, argv.data(), environ);
        ::posix_spawn_file_actions_destroy(&actions); ASSERT_EQ(spawned, 0);
        auto connection = BusClient::private_bus(address, *store);
        ASSERT_TRUE(connection); client = std::move(connection.value());
        ASSERT_TRUE(eventually([this] { return client->status(Action::ap_status).has_value(); }));
    }
    void absent(std::string_view bytes, const char* surface) const {
        std::string hex;
        constexpr std::string_view digits{"0123456789abcdef"};
        for (const char character : sentinel) {
            const auto byte = static_cast<unsigned char>(character);
            hex += digits[byte >> 4U]; hex += digits[byte & 15U];
        }
        auto* encoded = g_base64_encode(reinterpret_cast<const guchar*>(sentinel.data()), sentinel.size());
        EXPECT_EQ(bytes.find(sentinel), std::string_view::npos) << surface << " raw secret disclosed";
        EXPECT_EQ(bytes.find(hex), std::string_view::npos) << surface << " hex secret disclosed";
        EXPECT_EQ(bytes.find(encoded), std::string_view::npos) << surface << " base64 secret disclosed";
        g_free(encoded);
    }
    void files_absent() const {
        for (const auto& file : std::filesystem::recursive_directory_iterator{directory}) {
            // Only the helper's root-only credential file is excluded from disk
            // scanning. Authenticated GetAPCredentialsV1 and RFCOMM WifiInfoResponse
            // are intentionally credential-bearing wire replies, not log surfaces.
            if (file.path() == directory / "state/ap.credentials" || !file.is_regular_file()) continue;
            absent(read_text(file.path()), file.path().filename().c_str());
        }
    }
    void surfaces(SecretDiagnostics& diagnostics, ipc::ControlService& control,
                  const ipc::test::RecordingEventSink& events) const {
        absent(diagnostics.recorded, "receiver diagnostics");
        const auto state = control.state();
        std::ostringstream status;
        status << ipc::to_string(state.projection) << state.consumer.value << control.ping();
        for (const auto& [key, value] : control.capabilities()) status << key << value;
        for (const auto& event : events.events) {
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, ipc::DiagnosticEmitted>)
                    status << ipc::to_string(value.severity) << value.event << value.subject;
                else if constexpr (std::is_same_v<T, ipc::StateChanged>)
                    status << ipc::to_string(value.projection) << value.consumer.value;
                else status << value.request.value;
            }, event);
        }
        absent(status.str(), "control status and events"); files_absent();
    }
    void TearDown() override {
        daemon.stop();
        if (!directory.empty() && !sentinel.empty()) files_absent();
        NmDaemon::TearDown();
    }
};
}
}
