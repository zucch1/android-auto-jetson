// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/Bootstrap.hpp>
#include "Bootstrap.pb.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <regex>
#include <functional>
#include <string_view>
#include <thread>
#include <atomic>
#include <future>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace aa::helper {
namespace {
using namespace std::chrono_literals;
class BootstrapTest : public ::testing::Test {
protected:
    int sockets[2]{-1, -1};
    int sender{-1};
    Bootstrap::Time now{std::chrono::steady_clock::now()};
    ApSnapshot snapshot{{"ssid", "secret-sentinel", "02:ab:cd:ef:01:23", "10.42.0.1", 5277, 42}};
    std::unique_ptr<Bootstrap> bootstrap;
    void SetUp() override {
        ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets), 0);
        sender = sockets[0];
        bootstrap = std::make_unique<Bootstrap>(sockets[0], now);
        sockets[0] = -1;
    }
    void TearDown() override { bootstrap.reset(); for (int fd : sockets) if (fd >= 0) ::close(fd); }
    void send(std::uint16_t id, const std::string& payload = {}) {
        const auto bytes = encode_aaw(id, {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
        ASSERT_TRUE(bytes); ASSERT_EQ(::write(sockets[1], bytes->data(), bytes->size()), static_cast<ssize_t>(bytes->size()));
    }
    std::vector<std::uint8_t> read() {
        std::vector<std::uint8_t> bytes(8192);
        const auto n = ::recv(sockets[1], bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (n <= 0) return {};
        bytes.resize(static_cast<std::size_t>(n)); return bytes;
    }
    bool fence_current{true};
    void tick() { bootstrap->tick(now, [this] { return core::Result<ApSnapshot>{snapshot}; },
        [this](std::uint64_t, const std::function<void()>& send) { if (!fence_current) return false; send(); return true; }); }
    // Byte-exact fixtures in the shipping start-first layout (#166). Field numbers and IDs:
    // aa-proxy-rs f80236f src/protos/WifiStartRequest.proto:4-7, WifiInfoResponse.proto:22-28,
    // src/bluetooth.rs:409-420, 914-923; WirelessAndroidAutoDongle ef64522 bluetoothProfiles.cpp:68-77.
    static std::vector<std::uint8_t> bytes(std::string_view text) { return {text.begin(), text.end()}; }
    // WifiStartRequest=1 {1: "10.42.0.1", 2: 5277}
    static std::vector<std::uint8_t> start_request() {
        return bytes(std::string_view{"\x00\x0e\x00\x01" "\x0a\x09" "10.42.0.1" "\x10\x9d\x29", 18});
    }
    // WifiInfoResponse=3 {1: ssid, 2: key, 3: bssid, 4: WPA2_PERSONAL=8, 5: DYNAMIC=1}
    static std::vector<std::uint8_t> info_response() {
        return bytes(std::string_view{"\x00\x2e\x00\x03" "\x0a\x04" "ssid" "\x12\x0f" "secret-sentinel"
            "\x1a\x11" "02:ab:cd:ef:01:23" "\x20\x08" "\x28\x01", 50});
    }
    void raw(std::string_view wire) {
        ASSERT_EQ(::write(sockets[1], wire.data(), wire.size()), static_cast<ssize_t>(wire.size()));
    }
    void start() {
        ASSERT_TRUE(bootstrap->authorize(snapshot, true, now)); tick();
        // The HU opens with WifiStartRequest and nothing else: no version request, no setup info.
        ASSERT_EQ(read(), start_request());
    }
    std::string failure_log(const std::function<void()>& action) {
        ::testing::internal::CaptureStderr(); action(); return ::testing::internal::GetCapturedStderr();
    }
    // No SSID, PSK, BSSID, IP, port or any MAC-shaped address (phone or adapter) in a log line.
    void expect_secret_free(const std::string& log) {
        for (const auto& secret : {snapshot.ssid, snapshot.password, snapshot.bssid, snapshot.ip, std::to_string(snapshot.port)})
            EXPECT_EQ(log.find(secret), std::string::npos) << log;
        EXPECT_FALSE(std::regex_search(log, std::regex{"([0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}"})) << log;
    }
    void info() { raw(std::string_view{"\x00\x00\x00\x02", 4}); tick(); ASSERT_EQ(read(), info_response()); }
    std::size_t lines(const std::string& log, std::string_view needle) {
        std::size_t count{}; for (auto at = log.find(needle); at != std::string::npos; at = log.find(needle, at + 1)) ++count;
        return count;
    }
};
TEST(AawPayload, SecurityAndAccessPointBytesAreNotProjectionEnum) {
    aaw::WifiInfoResponse response;
    response.set_ssid("s"); response.set_password("p"); response.set_bssid("b");
    response.set_security_mode(aaw::WPA2_PERSONAL); response.set_access_point_type(aaw::DYNAMIC);
    EXPECT_EQ(response.SerializeAsString(), std::string("\x0a\x01s\x12\x01p\x1a\x01\x62\x20\x08\x28\x01", 13));
}
TEST_F(BootstrapTest, ReadyBeforeEndpointAndOrderedSuccess) {
    tick(); EXPECT_TRUE(read().empty());
    start(); send(2); tick();
    AawDecoder decoder; decoder.feed(read()); const auto record = decoder.next();
    ASSERT_TRUE(std::holds_alternative<AawRecord>(record)); EXPECT_EQ(std::get<AawRecord>(record).id, 3);
    aaw::WifiInfoResponse info; ASSERT_TRUE(info.ParseFromArray(std::get<AawRecord>(record).payload.data(), static_cast<int>(std::get<AawRecord>(record).payload.size())));
    EXPECT_TRUE(info.password() == snapshot.password); EXPECT_EQ(info.security_mode(), 8); EXPECT_EQ(info.access_point_type(), 1);
    EXPECT_TRUE(std::holds_alternative<std::monostate>(decoder.next()));
    aaw::WifiStartResponse started; started.set_status(aaw::STATUS_SUCCESS); send(7, started.SerializeAsString());
    aaw::WifiConnectStatus connected; connected.set_status(aaw::STATUS_SUCCESS); send(6, connected.SerializeAsString()); tick();
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::none); EXPECT_TRUE(bootstrap->complete());
}
TEST_F(BootstrapTest, StartFirstExchangeIsByteExactAndCompletes) {
    // Given: an authorized HU that has sent exactly WifiStartRequest.
    start();
    // When: the phone asks for the AP (WifiInfoRequest=2, empty).
    raw(std::string_view{"\x00\x00\x00\x02", 4}); tick();
    // Then: the HU answers with exactly WifiInfoResponse and no second start request.
    EXPECT_EQ(read(), info_response()); EXPECT_TRUE(bootstrap->live());
    // When: the phone reports start and connect success (status tag 3 and tag 1, value 0).
    std::string log = failure_log([&] { raw(std::string_view{"\x00\x02\x00\x07\x18\x00" "\x00\x02\x00\x06\x08\x00", 12}); tick(); });
    // Then: the bootstrap completes, nothing else is sent, and the channel is kept (#161).
    EXPECT_TRUE(bootstrap->complete()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
    EXPECT_NE(log.find("wireless_handshake_complete flow=start_first"), std::string::npos) << log;
    EXPECT_EQ(log.find("wireless_handshake_failed"), std::string::npos) << log;
    expect_secret_free(log);
    EXPECT_TRUE(read().empty()); EXPECT_FALSE(bootstrap->live()); EXPECT_TRUE(bootstrap->awaiting_confirmation());
}
TEST_F(BootstrapTest, PhoneStatusFixturesWithEveryFieldDecode) {
    // Synthetic fixtures built from the cited layouts (no phone capture exists):
    // WifiStartResponse=7 {1: ip, 2: port, 3: 0} and WifiConnectStatus=6 {1: 0, 2: ""}.
    start(); raw(std::string_view{"\x00\x00\x00\x02", 4}); tick(); ASSERT_EQ(read(), info_response());
    raw(std::string_view{"\x00\x10\x00\x07" "\x0a\x09" "10.42.0.1" "\x10\x9d\x29" "\x18\x00", 20});
    raw(std::string_view{"\x00\x04\x00\x06" "\x08\x00" "\x12\x00", 8}); tick();
    EXPECT_TRUE(bootstrap->complete()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
}
TEST_F(BootstrapTest, NegativeConnectStatusIsReportedByNumber) {
    // aa-proxy-rs bluetooth.rs:4746-4754: a phone that cannot join the AP sends status -3 as a 10-byte varint.
    start(); raw(std::string_view{"\x00\x00\x00\x02", 4}); tick(); read();
    const auto log = failure_log([&] {
        raw(std::string_view{"\x00\x02\x00\x07\x18\x00" "\x00\x0b\x00\x06" "\x08\xfd\xff\xff\xff\xff\xff\xff\xff\xff\x01", 21}); tick();
    });
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::phone_rejected); EXPECT_FALSE(bootstrap->complete());
    EXPECT_NE(log.find("phase=await_connect_status msg_id=6 msg=WifiConnectStatus reason=phone_rejected detail=\"phone status -3\""),
        std::string::npos) << log;
    expect_secret_free(log);
}
TEST_F(BootstrapTest, OldShiftedConnectStatusIsRejected) {
    // The pre-#166 layout put WifiConnectStatus status at tag 2 (varint 12); tag 2 is the string error message.
    start(); raw(std::string_view{"\x00\x00\x00\x02", 4}); tick(); read();
    raw(std::string_view{"\x00\x02\x00\x07\x18\x00" "\x00\x02\x00\x06\x10\x0c", 12}); tick();
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::protobuf); EXPECT_FALSE(bootstrap->complete());
}
TEST_F(BootstrapTest, VersionResponseIsUnexpectedAndReleasesNoSecret) {
    // A phone WifiVersionResponse=5 {1: 6, 2: 1, 4: 1} has no place in the start-first flow.
    start();
    const auto log = failure_log([&] { raw(std::string_view{"\x00\x06\x00\x05\x08\x06\x10\x01\x20\x01", 10}); tick(); });
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::wrong_order); EXPECT_TRUE(read().empty());
    EXPECT_NE(log.find("phase=await_info_request msg_id=5 msg=WifiVersionResponse reason=wrong_order"), std::string::npos) << log;
    expect_secret_free(log);
}
TEST_F(BootstrapTest, GarbledMessageLogsReasonAndDenies) {
    start();
    const auto log = failure_log([&] { raw(std::string_view{"\x00\x01\x00\x02\x80", 5}); tick(); });
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::protobuf); EXPECT_TRUE(read().empty());
    EXPECT_NE(log.find("phase=await_info_request msg_id=2 msg=WifiInfoRequest reason=protobuf detail=\"payload did not parse\""),
        std::string::npos) << log;
    expect_secret_free(log);
    char byte{}; EXPECT_EQ(::read(sockets[1], &byte, 1), 0);
}
TEST_F(BootstrapTest, OldShiftedStatusesAreRejectedAtConnectStatus) {
    // The pre-#166 layout put WifiStartResponse status at tag 4 (12) and WifiConnectStatus status at
    // tag 2 (varint 12). The first now reads as "no status" (tolerated); the second cannot parse.
    start(); info();
    const auto log = failure_log([&] {
        raw(std::string_view{"\x00\x02\x00\x07\x20\x0c", 6}); tick();
        EXPECT_TRUE(bootstrap->live());
        raw(std::string_view{"\x00\x02\x00\x06\x10\x0c", 6}); tick();
    });
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::protobuf); EXPECT_FALSE(bootstrap->complete());
    EXPECT_NE(log.find("wireless_handshake_accepted phase=await_start_response msg_id=7 msg=WifiStartResponse detail=\"no status\""),
        std::string::npos) << log;
    EXPECT_NE(log.find("phase=await_connect_status msg_id=6 msg=WifiConnectStatus reason=protobuf"), std::string::npos) << log;
    expect_secret_free(log);
}
TEST_F(BootstrapTest, StartResponseStatusOneIsAccepted) {
    // OAA maps 1 to success (WifiVersionStatusEnum.proto:18-31); no reference fails on it.
    start(); info();
    const auto log = failure_log([&] { raw(std::string_view{"\x00\x02\x00\x07\x18\x01" "\x00\x02\x00\x06\x08\x00", 12}); tick(); });
    EXPECT_TRUE(bootstrap->complete()) << log; EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
    EXPECT_NE(log.find("msg_id=7 msg=WifiStartResponse detail=\"status 1\""), std::string::npos) << log;
}
TEST_F(BootstrapTest, StartResponseWithoutStatusIsAccepted) {
    start(); info();
    raw(std::string_view{"\x00\x00\x00\x07" "\x00\x02\x00\x06\x08\x00", 10}); tick();
    EXPECT_TRUE(bootstrap->complete()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
}
TEST_F(BootstrapTest, ConnectStatusMustCarryItsStatus) {
    // aa-proxy-rs reads a phone's [08 00] as success (bluetooth.rs:4743-4753): stay strict here.
    start(); info(); raw(std::string_view{"\x00\x00\x00\x07", 4}); tick(); EXPECT_TRUE(bootstrap->live());
    raw(std::string_view{"\x00\x00\x00\x06", 4}); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::protobuf);
}
TEST_F(BootstrapTest, ConnectStatusOneIsRejected) {
    start(); info(); raw(std::string_view{"\x00\x02\x00\x07\x18\x00" "\x00\x02\x00\x06\x08\x01", 12}); tick();
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::phone_rejected); EXPECT_FALSE(bootstrap->complete());
}
TEST_F(BootstrapTest, PingsSetupInfoAndUnknownIdsAreIgnoredWithOneLineEach) {
    // Given: the phone interleaves a ping, a ping response, a SetupInfo and an unknown ID.
    start();
    const auto log = failure_log([&] {
        raw(std::string_view{"\x00\x02\x00\x08\x08\x01" "\x00\x00\x00\x09" "\x00\x00\x00\x0b" "\x00\x00\x00\x63" "\x00\x00\x00\x02", 22});
        tick();
        EXPECT_EQ(read(), info_response());
        // A ping during the Wi-Fi join, before the statuses.
        raw(std::string_view{"\x00\x02\x00\x07\x18\x00" "\x00\x02\x00\x08\x08\x02" "\x00\x02\x00\x06\x08\x00", 18}); tick();
    });
    // Then: each is one diagnostic line with its ID only, the exchange completes, nothing else is sent.
    EXPECT_TRUE(bootstrap->complete()) << log; EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
    EXPECT_EQ(lines(log, "system-helper: wireless_handshake_ignored "), 5U) << log;
    for (const auto* id : {"msg_id=8 msg=WifiPingRequest", "msg_id=9 msg=WifiPingResponse", "msg_id=11 msg=WifiSetupInfo", "msg_id=99 msg=unknown"})
        EXPECT_NE(log.find(id), std::string::npos) << id << '\n' << log;
    EXPECT_EQ(lines(log, "wireless_handshake_failed"), 0U) << log;
    expect_secret_free(log); EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, EarlyPhonePingIsJudgedAfterTheStartRequest) {
    // Bytes before authorization stay queued; they are not a failure (the dongles never check).
    raw(std::string_view{"\x00\x00\x00\x08", 4}); tick();
    EXPECT_TRUE(bootstrap->live()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
    start(); EXPECT_TRUE(bootstrap->live()); info(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
}
TEST_F(BootstrapTest, IgnoredRecordsAreBoundedAndThenLogAReasonAndDeny) {
    start();
    std::string wire;
    for (unsigned ping = 0; ping <= Bootstrap::kIgnoredLimit; ++ping) wire.append("\x00\x00\x00\x63", 4);
    const auto log = failure_log([&] { raw(wire); tick(); });
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::unknown_id); EXPECT_TRUE(read().empty());
    EXPECT_EQ(lines(log, "wireless_handshake_ignored"), Bootstrap::kIgnoredLimit) << log;
    EXPECT_NE(log.find("system-helper: wireless_handshake_failed phase=await_info_request msg_id=99 msg=unknown reason=unknown_id"
        " detail=\"too many ignored records\""), std::string::npos) << log;
    expect_secret_free(log);
    char byte{}; EXPECT_EQ(::read(sockets[1], &byte, 1), 0);
}
TEST_F(BootstrapTest, RepeatedInfoRequestGetsTheIdenticalResponseBounded) {
    // aa-proxy-rs re-sends its cached WifiInfoResponse (bluetooth.rs:1069-1086).
    start(); info();
    std::string log = failure_log([&] {
        raw(std::string_view{"\x00\x00\x00\x02", 4}); tick(); EXPECT_EQ(read(), info_response());
        // Also once the phone has answered the start (waiting for its Wi-Fi join).
        raw(std::string_view{"\x00\x02\x00\x07\x18\x00" "\x00\x00\x00\x02", 10}); tick(); EXPECT_EQ(read(), info_response());
    });
    EXPECT_TRUE(bootstrap->live()) << log;
    EXPECT_EQ(lines(log, "system-helper: wireless_handshake_resent phase="), Bootstrap::kInfoResendLimit) << log;
    expect_secret_free(log);
    log = failure_log([&] { raw(std::string_view{"\x00\x00\x00\x02", 4}); tick(); });
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::duplicate); EXPECT_TRUE(read().empty());
    EXPECT_NE(log.find("msg_id=2 msg=WifiInfoRequest reason=duplicate"), std::string::npos) << log;
}
TEST_F(BootstrapTest, RecordsAfterSuccessfulConnectStatusAreIgnored) {
    // A full record and a partial header coalesced after success: the exchange is still complete.
    start(); info();
    raw(std::string_view{"\x00\x02\x00\x07\x18\x00" "\x00\x02\x00\x06\x08\x00" "\x00\x02\x00\x08\x08\x01" "\x00", 19}); tick();
    EXPECT_TRUE(bootstrap->complete()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
    EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, ConnectionRejectionFailsAsPhoneRejected) {
    start(); info();
    const auto log = failure_log([&] { raw(std::string_view{"\x00\x02\x00\x0a\x08\x01", 6}); tick(); });
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::phone_rejected);
    EXPECT_NE(log.find("msg_id=10 msg=WifiConnectionRejection reason=phone_rejected"), std::string::npos) << log;
    expect_secret_free(log);
}
TEST_F(BootstrapTest, UnknownIdHeaderIsIgnoredButOversizeFailsClosed) {
    start(); const unsigned char header[]{0, 0, 0, 99}; ASSERT_EQ(::write(sockets[1], header, 4), 4);
    tick(); EXPECT_TRUE(bootstrap->live()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
    const unsigned char oversize[]{0x10, 1, 0, 99}; ASSERT_EQ(::write(sockets[1], oversize, 4), 4);
    tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::oversize);
}
TEST_F(BootstrapTest, DenyClosesWithoutPayload) {
    bootstrap->deny(HandoffFailure::denied); EXPECT_TRUE(read().empty());
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::denied); char byte{}; EXPECT_EQ(::read(sockets[1], &byte, 1), 0);
}
TEST_F(BootstrapTest, MissingListenerReadinessClosesWithoutEndpoint) {
    EXPECT_FALSE(bootstrap->authorize(snapshot, false, now));
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::eligibility); EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, FragmentedInfoIsAccumulated) {
    start(); const unsigned char header[]{0, 0, 0, 2};
    ASSERT_EQ(::write(sockets[1], header, 1), 1); tick(); EXPECT_TRUE(read().empty());
    ASSERT_EQ(::write(sockets[1], header + 1, 3), 3); tick(); EXPECT_FALSE(read().empty());
}
TEST_F(BootstrapTest, CoalescedTrailingMessagesAreValidated) {
    start(); send(2); tick(); read();
    const unsigned char bytes[]{0, 2, 0, 7, 0x18, 0, 0, 2, 0, 6, 8, 0};
    ASSERT_EQ(::write(sockets[1], bytes, sizeof(bytes)), static_cast<ssize_t>(sizeof(bytes))); tick();
    EXPECT_TRUE(bootstrap->complete());
}
TEST_F(BootstrapTest, BufferedStatusesDrainAfterCredentialBackpressure) {
    // Given: endpoint sent, but the credential response cannot be written.
    start();
    std::array<std::uint8_t, 4096> fill{};
    while (::send(sender, fill.data(), fill.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {}
    const unsigned char bytes[]{0, 0, 0, 2, 0, 2, 0, 7, 0x18, 0, 0, 2, 0, 6, 8, 0};
    ASSERT_EQ(::write(sockets[1], bytes, sizeof(bytes)), static_cast<ssize_t>(sizeof(bytes)));
    tick(); ASSERT_TRUE(bootstrap->live());
    while (!read().empty()) {}
    // When: backpressure clears without any new phone input.
    tick();
    // Then: the already buffered statuses complete the exchange.
    EXPECT_TRUE(bootstrap->complete()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
}
TEST_F(BootstrapTest, QueuedTrailingByteAtReceiveBoundaryIsIgnoredAfterSuccess) {
    // Given: successful statuses fill exactly one recv, with an extra byte already queued.
    start(); send(2); tick(); read();
    aaw::WifiConnectStatus connected; connected.set_status(aaw::STATUS_SUCCESS);
    connected.set_error_message(std::string(4085, 'x'));
    const auto payload = connected.SerializeAsString(); ASSERT_EQ(payload.size(), 4090U);
    const auto status = encode_aaw(6, {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
    ASSERT_TRUE(status);
    std::vector<std::uint8_t> bytes{0, 2, 0, 7, 0x18, 0};
    bytes.insert(bytes.end(), status->begin(), status->end()); ASSERT_EQ(bytes.size(), 4100U);
    bytes.push_back(0);
    ASSERT_EQ(::write(sockets[1], bytes.data(), bytes.size()), static_cast<ssize_t>(bytes.size()));
    // When: the first receive ends exactly after WifiConnectStatus.
    tick();
    // Then: the exchange is complete; the queued byte is never read (#170 review), and a repeated
    // tick or authorization changes nothing.
    EXPECT_TRUE(bootstrap->complete()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
    EXPECT_FALSE(bootstrap->expire()); EXPECT_TRUE(bootstrap->authorize(snapshot, true, now));
    tick(); EXPECT_TRUE(bootstrap->complete()); EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, DeadlineCrossedDuringProcessingCannotCreateAuthority) {
    // Given: info and both statuses are buffered before the credential send crosses the deadline.
    const auto accepted = now;
    std::atomic<std::int64_t> elapsed{};
    // A fresh pair: destroying the fixture's unfinished bootstrap shuts its socket down (#161).
    bootstrap.reset();
    int pair[2]{}; ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    ::close(sockets[1]); sockets[1] = pair[1]; sender = pair[0];
    bootstrap = std::make_unique<Bootstrap>(sender, accepted, [&] { return accepted + std::chrono::milliseconds{elapsed.load()}; });
    start(); send(2);
    aaw::WifiStartResponse started; started.set_status(aaw::STATUS_SUCCESS); send(7, started.SerializeAsString());
    aaw::WifiConnectStatus connected; connected.set_status(aaw::STATUS_SUCCESS); send(6, connected.SerializeAsString());
    // When: processing advances the same injected clock after tick's initial expiry check.
    bootstrap->tick(now, [this] { return core::Result<ApSnapshot>{snapshot}; },
        [&](std::uint64_t, const std::function<void()>& send) { send(); elapsed = 10000; return true; });
    // Then: timeout is terminal, retains the original deadline, and never becomes unlimited authority.
    EXPECT_EQ(elapsed.load(), 10000); EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout);
    EXPECT_FALSE(bootstrap->complete()); EXPECT_FALSE(bootstrap->live()); EXPECT_TRUE(bootstrap->expire());
    EXPECT_EQ(bootstrap->deadline().until, accepted + Bootstrap::kBudget);
    elapsed = 0; tick(); EXPECT_FALSE(bootstrap->authorize(snapshot, true, now));
    EXPECT_FALSE(bootstrap->complete()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout);
    bootstrap.reset();
}
TEST_F(BootstrapTest, DelayedSnapshotCannotDelaySocketDeadline) {
    // Given: only 80ms remain when a snapshot provider starts a blocked collection.
    bootstrap.reset();
    int pair[2]{}; ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    ::close(sockets[1]); sockets[1] = pair[1]; sender = pair[0];
    now = std::chrono::steady_clock::now();
    bootstrap = std::make_unique<Bootstrap>(sender, now - Bootstrap::kBudget + 80ms);
    ASSERT_TRUE(bootstrap->authorize(snapshot, true, now));
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    auto work = std::async(std::launch::async, [&] {
        bootstrap->tick(now, [&] { entered.set_value(); released.wait(); return core::Result<ApSnapshot>{snapshot}; },
            [](std::uint64_t, const std::function<void()>& send) { send(); return true; });
    });
    entered.get_future().wait();
    // When: collection remains blocked across the deadline (no further tick).
    ::pollfd event{sockets[1], POLLIN, 0}; const auto closed = ::poll(&event, 1, 200);
    release.set_value(); work.get();
    // Then: the peer observes closure while collection is pending, without credential bytes.
    EXPECT_EQ(closed, 1); EXPECT_TRUE(read().empty());
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout);
}
TEST_F(BootstrapTest, ConnectStatusRejectionIsNotSuccess) {
    start(); send(2); tick(); read();
    aaw::WifiStartResponse started; started.set_status(aaw::STATUS_SUCCESS); send(7, started.SerializeAsString());
    aaw::WifiConnectStatus connected; connected.set_status(aaw::STATUS_WIFI_DISABLED); send(6, connected.SerializeAsString()); tick();
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::phone_rejected); EXPECT_FALSE(bootstrap->complete());
}
TEST_F(BootstrapTest, WriteFailureClosesAndDoesNotRetry) {
    ASSERT_TRUE(bootstrap->authorize(snapshot, true, now)); ::shutdown(sockets[1], SHUT_RDWR); tick();
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::write); EXPECT_FALSE(bootstrap->live());
}
TEST_F(BootstrapTest, SilentPeerAfterEndpointHasSameWholeDeadline) {
    start(); now += 10s; tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout);
}
TEST_F(BootstrapTest, BackpressuredWriteCannotExtendBudgetOrBlockCancellation) {
    std::vector<std::uint8_t> fill(4096, 0);
    while (::send(sender, fill.data(), fill.size(), MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {}
    ASSERT_TRUE(bootstrap->authorize(snapshot, true, now));
    const auto began = std::chrono::steady_clock::now(); tick();
    EXPECT_LT(std::chrono::steady_clock::now() - began, 1s); EXPECT_TRUE(bootstrap->live());
    now += 10s; tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout); EXPECT_FALSE(bootstrap->live());
}
TEST_F(BootstrapTest, WholeBudgetIncludesAuthorizationWait) {
    now += 10s; tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout);
    EXPECT_FALSE(bootstrap->authorize(snapshot, true, now));
}
TEST_F(BootstrapTest, WrongOrderAndDuplicateNeverReleaseFurtherSecrets) {
    start(); send(7); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::wrong_order); EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, ParseFailureHasNoPayloadDiagnostic) {
    start(); send(2, std::string("\x80", 1)); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::protobuf); EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, GenerationChangeBeforeInfoPreventsSecrets) {
    start(); ++snapshot.generation; send(2); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::snapshot); EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, GenerationFenceBetweenSnapshotAndSendPreventsBytes) {
    start(); fence_current = false; send(2); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::snapshot); EXPECT_TRUE(read().empty());
}
TEST_F(BootstrapTest, PhoneRejectionIsValidated) {
    start(); send(2); tick(); read(); aaw::WifiStartResponse rejected; rejected.set_status(aaw::STATUS_WIFI_DISABLED);
    send(7, rejected.SerializeAsString()); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::phone_rejected);
}
TEST_F(BootstrapTest, EofAndTruncationAreDifferentTypedFailures) {
    start(); const char partial = 0; ASSERT_EQ(::write(sockets[1], &partial, 1), 1);
    ::shutdown(sockets[1], SHUT_WR); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::truncated);
}
TEST_F(BootstrapTest, EofFailsWithoutCredentials) {
    start(); ::shutdown(sockets[1], SHUT_WR); tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::eof);
}
TEST_F(BootstrapTest, OversizeFailsBeforeReadingPayload) {
    start(); const unsigned char header[]{0x10, 1, 0, 2}; ASSERT_EQ(::write(sockets[1], header, 4), 4);
    tick(); EXPECT_EQ(bootstrap->failure(), HandoffFailure::oversize);
}
TEST_F(BootstrapTest, SingleTransitionYieldsBeforeCredentialValidationAndRetainsFinalFence) {
    start(); send(2);
    unsigned collections{}, fences{};
    const auto step = [&] {
        bootstrap->tick(now, [&] { ++collections; return core::Result<ApSnapshot>{snapshot}; },
            [&](std::uint64_t, const std::function<void()>& send) { ++fences; if (!fence_current) return false; send(); return true; }, true);
    };
    step(); EXPECT_TRUE(read().empty()); EXPECT_EQ(collections, 0U);
    fence_current = false;
    step(); EXPECT_EQ(collections, 1U); EXPECT_EQ(fences, 1U);
    EXPECT_TRUE(read().empty()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::snapshot);
}
}
// #161: bluetoothd keeps its own reference to every NewConnection socket. `daemon` models it
// (a dup of the helper's end), so the phone end sees EOF only when the helper shuts the shared
// socket down: an AF_UNIX shutdown reaches every fd of that end, as an RFCOMM shutdown sends DISC.
class BlueZHeldBootstrapTest : public BootstrapTest {
protected:
    int daemon{-1};
    std::atomic<std::int64_t> elapsed{};
    void SetUp() override {
        BootstrapTest::SetUp();
        bootstrap.reset();
        int pair[2]{}; ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
        ::close(sockets[1]); sockets[1] = pair[1]; sender = pair[0];
        daemon = ::dup(pair[0]); ASSERT_GE(daemon, 0);
        const auto accepted = now;
        bootstrap = std::make_unique<Bootstrap>(pair[0], accepted, [this, accepted] {
            return accepted + std::chrono::milliseconds{elapsed.load()}; });
    }
    void TearDown() override { BootstrapTest::TearDown(); if (daemon >= 0) ::close(daemon); }
    // The phone end reads EOF now (no data, no wait).
    bool phone_eof() {
        char byte{};
        return ::recv(sockets[1], &byte, 1, MSG_DONTWAIT) == 0;
    }
    void complete() {
        start(); send(2); tick(); read();
        aaw::WifiStartResponse started; started.set_status(aaw::STATUS_SUCCESS); send(7, started.SerializeAsString());
        aaw::WifiConnectStatus connected; connected.set_status(aaw::STATUS_SUCCESS); send(6, connected.SerializeAsString());
        tick(); ASSERT_TRUE(bootstrap->complete());
    }
};
TEST_F(BlueZHeldBootstrapTest, DenyShutsTheSharedChannelDown) {
    EXPECT_FALSE(phone_eof());
    bootstrap->deny(HandoffFailure::eligibility);
    EXPECT_TRUE(phone_eof()) << "close() alone leaves BlueZ's reference open: no DISC";
}
TEST_F(BlueZHeldBootstrapTest, DestroyingAnUnfinishedBootstrapShutsTheChannelDown) {
    ASSERT_TRUE(bootstrap->authorize(snapshot, true, now));
    bootstrap.reset();
    EXPECT_TRUE(phone_eof());
}
TEST_F(BlueZHeldBootstrapTest, PhoneFailureShutsTheChannelDown) {
    start(); send(7); tick();
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::wrong_order);
    EXPECT_TRUE(phone_eof());
}
TEST_F(BlueZHeldBootstrapTest, CompletedExchangeKeepsTheChannelUntilAbortedAfterwards) {
    complete();
    EXPECT_FALSE(phone_eof()) << "success leaves the channel as before";
    // The hand-off fails after the last RFCOMM message (no TCP session): the phone is freed.
    bootstrap->deny(HandoffFailure::denied);
    EXPECT_FALSE(bootstrap->complete());
    EXPECT_TRUE(phone_eof());
}
TEST_F(BlueZHeldBootstrapTest, CompletedExchangeDestroyedUnconfirmedShutsTheChannelDown) {
    complete();
    bootstrap.reset();
    EXPECT_TRUE(phone_eof());
}
TEST_F(BlueZHeldBootstrapTest, ConfirmedCompletionIsNeverCutByTheBackstop) {
    // A healthy, confirmed session keeps the phone's channel past the 30 s backstop.
    complete();
    bootstrap->confirm();
    EXPECT_FALSE(bootstrap->awaiting_confirmation());
    elapsed = 40'000;
    EXPECT_FALSE(bootstrap->expire());
    EXPECT_FALSE(phone_eof()) << "the backstop cut a confirmed session";
    EXPECT_TRUE(bootstrap->complete());
    // Its retirement, later, still frees the phone.
    bootstrap->deny(HandoffFailure::denied);
    EXPECT_TRUE(phone_eof());
}
TEST_F(BlueZHeldBootstrapTest, UnconfirmedCompletionBackstopShutsTheChannelDownKeepingAuthority) {
    complete();
    elapsed = 29'000; EXPECT_FALSE(bootstrap->expire()); EXPECT_FALSE(phone_eof());
    elapsed = 40'000;
    EXPECT_FALSE(bootstrap->expire());
    EXPECT_TRUE(phone_eof());
    EXPECT_TRUE(bootstrap->complete()) << "completed authority has no TTL (contract v2.6)";
    EXPECT_FALSE(bootstrap->awaiting_confirmation());
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::none);
}
// #170 review: the phone's Wi-Fi join gets its own bound once the credentials are out.
TEST_F(BlueZHeldBootstrapTest, WifiJoinGetsItsOwnBoundAfterCredentials) {
    start(); send(2); tick(); ASSERT_EQ(read(), info_response());
    const auto accepted = now;
    EXPECT_EQ(bootstrap->deadline().until, accepted + Bootstrap::kJoinBudget);
    // Past the 10 s first phase: still running, and the expiry worker did not cut the socket.
    elapsed = 25'000; std::this_thread::sleep_for(20ms);
    EXPECT_FALSE(bootstrap->expire()); EXPECT_TRUE(bootstrap->live()); EXPECT_FALSE(phone_eof());
    aaw::WifiStartResponse started; started.set_status(aaw::STATUS_SUCCESS); send(7, started.SerializeAsString()); tick();
    EXPECT_TRUE(bootstrap->live());
    // The join bound is finite: at 30 s after the credentials the exchange fails and frees the phone.
    elapsed = 30'000;
    EXPECT_TRUE(bootstrap->expire()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout);
    EXPECT_TRUE(phone_eof());
}
// #170 review (Neon P3): the expiry worker crosses the first budget and shuts the socket down after
// flush()'s own deadline check, before the join extension. The exchange must then fail, not run on
// as a live bootstrap whose channel the phone has already lost.
TEST_F(BlueZHeldBootstrapTest, JoinExtensionAfterTheWorkerFiredFailsTheExchange) {
    bootstrap.reset();
    int pair[2]{}; ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), 0);
    ::close(sockets[1]); sockets[1] = pair[1]; sender = pair[0];
    ::close(daemon); daemon = ::dup(pair[0]); ASSERT_GE(daemon, 0);
    const auto accepted = now;
    const auto owner = std::this_thread::get_id();
    std::atomic<bool> crossed{};
    // This thread still reads 1 ms inside the first budget; once crossed, the worker reads it as passed.
    bootstrap = std::make_unique<Bootstrap>(pair[0], accepted, [&crossed, accepted, owner] {
        return accepted + Bootstrap::kBudget - (std::this_thread::get_id() != owner && crossed.load() ? 0ms : 1ms);
    });
    start(); send(2);
    bool eof{};
    bootstrap->tick(now, [this] { return core::Result<ApSnapshot>{snapshot}; },
        [&](std::uint64_t, const std::function<void()>& send) {
            send(); crossed = true;
            // Hold the send's return until the worker has shut the channel down (the phone reads EOF).
            const auto until = std::chrono::steady_clock::now() + 2s;
            while (!eof && std::chrono::steady_clock::now() < until) {
                char bytes[256];
                const auto n = ::recv(sockets[1], bytes, sizeof bytes, MSG_DONTWAIT);
                if (n == 0) eof = true; else if (n < 0) std::this_thread::sleep_for(1ms);
            }
            return true;
        });
    ASSERT_TRUE(eof) << "the expiry worker never fired";
    EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout);
    EXPECT_FALSE(bootstrap->live()) << "the exchange runs on after the phone's channel was shut down";
    bootstrap.reset(); // its clock borrows this test's locals
}
TEST_F(BlueZHeldBootstrapTest, StallBeforeCredentialsStillEndsAtTheFirstBudget) {
    start(); elapsed = 10'000;
    EXPECT_TRUE(bootstrap->expire()); EXPECT_EQ(bootstrap->failure(), HandoffFailure::timeout); EXPECT_TRUE(phone_eof());
}
TEST_F(BlueZHeldBootstrapTest, JoinCompletesWellAfterTheFirstBudget) {
    start(); send(2); tick(); read();
    elapsed = 28'000;
    aaw::WifiStartResponse started; started.set_status(aaw::STATUS_SUCCESS); send(7, started.SerializeAsString());
    aaw::WifiConnectStatus connected; connected.set_status(aaw::STATUS_SUCCESS); send(6, connected.SerializeAsString());
    tick(); EXPECT_TRUE(bootstrap->complete()); EXPECT_FALSE(phone_eof());
}

}
