// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/helper/nm/Platform.hpp"
#include "../../src/helper/crypto_adapter/Crypto.hpp"
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <aa/trust/PairingPolicy.hpp>
#include "../dbus/fakes.hpp"
#include "../trust/syscall_faults.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <array>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

extern char** environ;
namespace aa::helper::nm {
namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
constexpr auto root = "/org/freedesktop/NetworkManager";
constexpr auto dev = "/org/freedesktop/NetworkManager/Devices/1";
constexpr auto active = "/org/freedesktop/NetworkManager/ActiveConnection/1";
constexpr auto profile = "/org/freedesktop/NetworkManager/Settings/1";
constexpr auto manager = "org.freedesktop.NetworkManager";
template<class F> bool eventually(F check, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) { if (check()) return true; std::this_thread::sleep_for(10ms); }
    return check();
}
void save(const std::filesystem::path& path, const std::string& data, mode_t mode = 0600) {
    std::ofstream{path} << data;
    ASSERT_EQ(::chmod(path.c_str(), mode), 0);
}
std::string read_text(const std::filesystem::path& path) {
    std::ifstream file{path}; return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
struct Child final {
    pid_t pid{};
    int input{-1};
    void start(const std::vector<std::string>& args) {
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        ASSERT_EQ(::posix_spawn(&pid, argv[0], nullptr, nullptr, argv.data(), environ), 0);
    }
    void start_ready(const std::vector<std::string>& args) {
        int fd[2]; ASSERT_EQ(::pipe2(fd, O_CLOEXEC), 0);
        int commands[2]; ASSERT_EQ(::pipe2(commands, O_CLOEXEC), 0);
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, commands[0], STDIN_FILENO);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr);
        ASSERT_EQ(::posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ), 0);
        posix_spawn_file_actions_destroy(&actions); ::close(fd[1]);
        ::close(commands[0]); input = commands[1];
        std::string ready;
        char byte{};
        ::pollfd input{fd[0], POLLIN, 0};
        while (::poll(&input, 1, 3000) == 1 && ::read(fd[0], &byte, 1) == 1 && byte != '\n') ready += byte;
        ::close(fd[0]); ASSERT_EQ(ready, "ready");
    }
    void stop() {
        if (pid) { ::kill(pid, SIGTERM); EXPECT_TRUE(eventually([this] { return ::waitpid(pid, nullptr, WNOHANG) == pid; }, 8000ms)); pid = 0; }
        if (input >= 0) { ::close(input); input = -1; }
    }
    ~Child() { stop(); }
};
class Boundary final {
public:
    std::atomic<bool> table{}, country{true}, enabled{true}, wrong_channel{}, block_remove{}, fail_install{}, foreign_table{}, quarantined{}, fail_listing{};
    std::atomic<int> installations{}, removals{}, radios{}, calls{};
    std::mutex mutex;
    std::string rules;
    Boundary(const std::string& path) {
        socket_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        EXPECT_LT(path.size(), sizeof(address.sun_path));
        std::copy(path.begin(), path.end(), address.sun_path);
        EXPECT_EQ(::bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
        EXPECT_EQ(::listen(socket_, 10), 0);
        worker_ = std::jthread{[this](const std::stop_token& stop) {
            while (!stop.stop_requested()) {
                ::pollfd event{socket_, POLLIN, 0};
                if (::poll(&event, 1, 20) != 1) continue;
                const int fd = ::accept4(socket_, nullptr, nullptr, SOCK_CLOEXEC);
                if (fd < 0) continue;
                std::array<char, 8192> data{};
                const auto size = ::recv(fd, data.data(), data.size(), 0);
                std::string request = size > 0 ? std::string{data.data(), static_cast<std::size_t>(size)} : "";
                ++calls;
                std::string reply = "1";
                if (request == "/usr/sbin/iw\ndev\nwlan-test\ninfo\n\n") {
                    ++radios;
                    reply = "0Interface wlan-test\n\twiphy 0\n\ttype AP\n\tchannel " +
                        std::string{wrong_channel ? "44 (5220 MHz)" : "36 (5180 MHz)"} + ", width: 20 MHz\n";
                } else if (request == "/usr/sbin/iw\ndev\n\n") reply = "0phy#0\n\tInterface wlan-test\n";
                else if (request == "/usr/sbin/iw\nreg\nget\n\n") reply = country ? "0global\ncountry US: DFS-FCC\n" : "0global\ncountry 00: DFS-UNSET\n";
                else if (request == "/usr/sbin/iw\nphy\nphy0\ninfo\n\n") reply = enabled ? "0 * 5180 MHz [36] (23.0 dBm)\n" : "0 * 5180 MHz [36] (disabled)\n";
                else if (request == "/usr/sbin/nft\nlist\ntables\ninet\n\n") {
                    if (!fail_listing) reply = table ? "0table inet aa_receiver_ap\n" : "0";
                } else if (request == "/usr/sbin/nft\nlist\ntable\ninet\naa_receiver_ap\n\n") {
                    reply = table ? (foreign_table ? "0table inet aa_receiver_ap { comment \"foreign\"; }\n" :
                        "0table inet aa_receiver_ap { comment \"aa-system-helper-v1\"; }\n") : "1";
                } else if (request.starts_with("/usr/sbin/nft\n-f\n-\n\ncreate table inet aa_receiver_ap")) {
                    if (!table && !fail_install) {
                        const std::lock_guard lock{mutex}; rules = request;
                        table = true; quarantined = false; ++installations; reply = "0";
                    }
                } else if (request == "/usr/sbin/nft\n-f\n-\n\nflush chain inet aa_receiver_ap input\nadd rule inet aa_receiver_ap input iifname \"wlan-test\" drop\n") {
                    if (table) { quarantined = true; reply = "0"; }
                } else if (request == "/usr/sbin/nft\n-f\n-\n\ndelete table inet aa_receiver_ap\n") {
                    if (!block_remove) { table = false; ++removals; reply = "0"; }
                }
                (void)::send(fd, reply.data(), reply.size(), MSG_NOSIGNAL); ::close(fd);
            }
        }};
    }
    ~Boundary() { worker_.request_stop(); worker_.join(); ::close(socket_); }
private:
    int socket_{};
    std::jthread worker_;
};
constexpr auto xml = R"xml(<node>
<interface name="org.freedesktop.NetworkManager">
<method name="GetDeviceByIpIface"><arg type="s" direction="in"/><arg type="o" direction="out"/></method>
<method name="AddAndActivateConnection2"><arg type="a{sa{sv}}" direction="in"/><arg type="o" direction="in"/><arg type="o" direction="in"/><arg type="a{sv}" direction="in"/><arg type="o" direction="out"/><arg type="o" direction="out"/><arg type="a{sv}" direction="out"/></method>
<method name="DeactivateConnection"><arg type="o" direction="in"/></method>
</interface>
<interface name="org.freedesktop.NetworkManager.Device">
<property name="DeviceType" type="u" access="read"/><property name="Interface" type="s" access="read"/><property name="ActiveConnection" type="o" access="read"/>
</interface>
<interface name="org.freedesktop.NetworkManager.Connection.Active"><property name="State" type="u" access="read"/></interface>
<interface name="org.freedesktop.NetworkManager.Settings.Connection"><method name="Delete"/></interface>
</node>)xml";
class NetworkManager final {
public:
    std::atomic<bool> active_now{}, pending{}, deny{}, stall{}, fail_stop{}, late_reply{}, valid{true};
    std::atomic<int> starts{}, deactivations{}, deletions{}, disconnects{};
    Boundary& boundary;
    std::mutex mutex;
    std::string seen_psk;
    NetworkManager(const std::string& address, Boundary& commands) : boundary(commands) {
        std::promise<bool> ready;
        auto result = ready.get_future();
        worker_ = std::jthread{[this, address, ready = std::move(ready)](const std::stop_token& stop) mutable {
            context_ = g_main_context_new(); g_main_context_push_thread_default(context_);
            bus_ = g_dbus_connection_new_for_address_sync(address.c_str(),
                static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
            if (!bus_) { ready.set_value(false); return; }
            g_dbus_connection_set_exit_on_close(bus_, FALSE);
            auto* reply = g_dbus_connection_call_sync(bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                "RequestName", g_variant_new("(su)", manager, 4U), G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
            if (reply) g_variant_unref(reply);
            auto* node = g_dbus_node_info_new_for_xml(xml, nullptr);
            const GDBusInterfaceVTable vtable{method, property, nullptr, {nullptr}};
            const char* paths[]{root, dev, active, profile};
            for (unsigned i = 0; i < 4; ++i)
                EXPECT_NE(g_dbus_connection_register_object(bus_, paths[i], node->interfaces[i], &vtable, this, nullptr, nullptr), 0U);
            const auto subscription = g_dbus_connection_signal_subscribe(bus_, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE, owner_changed, this, nullptr);
            ready.set_value(true);
            while (!stop.stop_requested()) {
                while (g_main_context_pending(context_)) g_main_context_iteration(context_, FALSE);
                std::this_thread::sleep_for(1ms);
            }
            (void)g_dbus_connection_close_sync(bus_, nullptr, nullptr);
            g_dbus_connection_signal_unsubscribe(bus_, subscription);
            g_object_unref(bus_); bus_ = nullptr; g_dbus_node_info_unref(node);
            g_main_context_pop_thread_default(context_); g_main_context_unref(context_);
        }};
        EXPECT_TRUE(result.get());
    }
    void stop() { if (worker_.joinable()) { worker_.request_stop(); worker_.join(); } }
    ~NetworkManager() { stop(); }
private:
    GDBusConnection* bus_{};
    GMainContext* context_{};
    std::jthread worker_;
    std::string client_;
    static void owner_changed(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* args, gpointer data) {
        auto& self = *static_cast<NetworkManager*>(data);
        const char *name{}, *old{}, *next{}; g_variant_get(args, "(&s&s&s)", &name, &old, &next);
        if (self.client_ == name && *old && !*next && !self.fail_stop) {
            self.active_now = false; self.pending = false; ++self.disconnects;
        }
    }
    static GVariant* property(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**, gpointer data) {
        auto& self = *static_cast<NetworkManager*>(data);
        if (std::string{name} == "DeviceType") return g_variant_new_uint32(2);
        if (std::string{name} == "Interface") return g_variant_new_string("wlan-test");
        if (std::string{name} == "ActiveConnection") return g_variant_new_object_path(self.active_now || self.pending ? active : "/");
        if (std::string{name} == "State") return g_variant_new_uint32(self.active_now ? 2 : 1);
        return nullptr;
    }
    static void method(GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar* name,
                       GVariant* args, GDBusMethodInvocation* invocation, gpointer data) {
        auto& self = *static_cast<NetworkManager*>(data);
        const std::string member{name};
        if (member == "GetDeviceByIpIface") { g_dbus_method_invocation_return_value(invocation, g_variant_new("(o)", dev)); return; }
        if (member == "DeactivateConnection") {
            ++self.deactivations;
            if (self.fail_stop) { g_dbus_method_invocation_return_dbus_error(invocation, "org.example.Failed", "blocked"); return; }
            self.active_now = false; self.pending = false;
        } else if (member == "Delete") ++self.deletions;
        else if (member == "AddAndActivateConnection2") {
            ++self.starts; self.client_ = sender;
            if (!self.boundary.table) self.valid = false;
            GVariant *settings{}, *options{};
            const char *device_path{}, *specific{};
            g_variant_get(args, "(@a{sa{sv}}&o&o@a{sv})", &settings, &device_path, &specific, &options);
            self.valid = self.valid && std::string{device_path} == dev && std::string{specific} == "/";
            const char *persist{}, *bind{};
            self.valid = self.valid && g_variant_lookup(options, "persist", "&s", &persist) && std::string{persist} == "volatile" &&
                g_variant_lookup(options, "bind-activation", "&s", &bind) && std::string{bind} == "dbus-client";
            auto* wireless = g_variant_lookup_value(settings, "802-11-wireless", G_VARIANT_TYPE_VARDICT);
            auto* security = g_variant_lookup_value(settings, "802-11-wireless-security", G_VARIANT_TYPE_VARDICT);
            auto* ipv4 = g_variant_lookup_value(settings, "ipv4", G_VARIANT_TYPE_VARDICT);
            const char *mode{}, *band{}, *psk{}, *key{}, *ipv4_method{}; unsigned channel{};
            self.valid = self.valid && wireless && security && ipv4 &&
                g_variant_lookup(wireless, "mode", "&s", &mode) && std::string{mode} == "ap" &&
                g_variant_lookup(wireless, "band", "&s", &band) && std::string{band} == "a" &&
                g_variant_lookup(wireless, "channel", "u", &channel) && channel == 36 &&
                g_variant_lookup(security, "key-mgmt", "&s", &key) && std::string{key} == "wpa-psk" &&
                g_variant_lookup(security, "psk", "&s", &psk) && std::string{psk}.size() == 64 &&
                g_variant_lookup(ipv4, "method", "&s", &ipv4_method) && std::string{ipv4_method} == "shared";
            if (self.valid) { const std::lock_guard lock{self.mutex}; self.seen_psk = psk; }
            if (wireless) g_variant_unref(wireless);
            if (security) g_variant_unref(security);
            if (ipv4) g_variant_unref(ipv4);
            g_variant_unref(settings); g_variant_unref(options);
            if (self.deny) { g_dbus_method_invocation_return_dbus_error(invocation, "org.example.Denied", "denied"); return; }
            self.pending = true; self.active_now = !self.stall;
            if (self.late_reply) std::this_thread::sleep_for(450ms);
            GVariantBuilder result; g_variant_builder_init(&result, G_VARIANT_TYPE_VARDICT);
            g_dbus_method_invocation_return_value(invocation, g_variant_new("(ooa{sv})", profile, active, &result));
            return;
        }
        g_dbus_method_invocation_return_value(invocation, g_variant_new("()"));
    }
};
class NmDaemon : public ::testing::Test {
protected:
    std::filesystem::path directory;
    Child bus, daemon, bluez;
    std::string address;
    std::unique_ptr<Boundary> boundary;
    std::unique_ptr<NetworkManager> nm;
    std::unique_ptr<trust::ApprovedPhoneStore> store;
    std::unique_ptr<BusClient> client;
    void SetUp() override {
        ASSERT_NE(::getuid(), 0U);
        std::string name = std::string{AA_HELPER_BUILD} + "/nm-XXXXXX";
        ASSERT_NE(::mkdtemp(name.data()), nullptr); directory = name;
        std::filesystem::create_directory(directory / "state"); ::chmod((directory / "state").c_str(), 0700);
        save(directory / "ap.conf", "v1 wlan-test US jurisdiction-confirmed 5277\n");
        save(directory / "session.uid", std::to_string(::getuid()) + "\n");
        const int exe = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
        const auto digest = crypto_adapter::hash_executable(exe, Clock::now() + 1s); ::close(exe);
        ASSERT_TRUE(digest); save(directory / "receiver.sha256", digest.value() + "\n");
        int fd[2]; ASSERT_EQ(::pipe2(fd, O_CLOEXEC), 0);
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, fd[1], STDOUT_FILENO);
        char executable[] = "/usr/bin/dbus-daemon", session[] = "--session", nofork[] = "--nofork", print[] = "--print-address=1";
        char* args[]{executable, session, nofork, print, nullptr};
        ASSERT_EQ(posix_spawn(&bus.pid, executable, &actions, nullptr, args, environ), 0);
        posix_spawn_file_actions_destroy(&actions); ::close(fd[1]);
        char byte{};
        while (::read(fd[0], &byte, 1) == 1 && byte != '\n') address += byte;
        ::close(fd[0]); ASSERT_FALSE(address.empty());
        boundary = std::make_unique<Boundary>((directory / "commands").string());
        nm = std::make_unique<NetworkManager>(address, *boundary);
        bluez.start_ready({"/usr/bin/python3", "-B", std::string{AA_HELPER_SOURCE} + "/tests/helper/bluez_mock.py", address});
        store = std::make_unique<trust::ApprovedPhoneStore>(directory / "phones"); ASSERT_TRUE(store->load());
    }
    void start() {
        daemon.start({std::string{AA_HELPER_BUILD} + "/aa-system-helper", "--private-bus", address, directory.string(), "--combined", (directory / "commands").string()});
        auto connection = BusClient::private_bus(address, *store); ASSERT_TRUE(connection); client = std::move(connection.value());
        ASSERT_TRUE(eventually([this] { return client->status(Action::ap_status).has_value(); }));
    }
    void acquire(std::uint64_t generation = 1) {
        ASSERT_TRUE(client->prepare(core::ConsumerId{generation}, core::PairingRequestId{generation}));
        ASSERT_TRUE(client->confirmed(core::ConsumerId{generation}, core::PairingRequestId{generation}));
        core::CancellationSource cancel;
        const auto result = client->call({Operation::ensure_access_point, 36}, core::Milliseconds{1000}, cancel.get_token());
        ASSERT_TRUE(result);
    }
    bool state(State expected) {
        return eventually([this, expected] { const auto result = client->status(Action::ap_status); return result && result.value() == expected; }, 6500ms);
    }
    core::Result<void> revoke_ap(const RevocationV1& snapshot) {
        const auto report = client->forgotten_v1(snapshot, {});
        EXPECT_TRUE(report.bond.has_value());
        if (report.bond) { EXPECT_FALSE(*report.bond); }
        return report.ap;
    }
    void TearDown() override {
        client.reset(); daemon.stop(); bluez.stop(); nm.reset(); boundary.reset(); store.reset(); bus.stop();
        std::filesystem::remove_all(directory);
    }
};
TEST_F(NmDaemon, HappyStartProjectionIsolationAndRollback) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); EXPECT_TRUE(nm->valid);
    ASSERT_EQ(nm->starts, 1); ASSERT_TRUE(boundary->table);
    const auto text = read_text(directory / "state/ap.credentials"); EXPECT_EQ(text.size(), 85U);
    struct stat st{}; ASSERT_EQ(::stat((directory / "state/ap.credentials").c_str(), &st), 0); EXPECT_EQ(st.st_mode & 0777, 0600U);
    { const std::lock_guard lock{boundary->mutex};
      EXPECT_NE(boundary->rules.find("udp sport 68 udp dport 67 accept"), std::string::npos);
      EXPECT_NE(boundary->rules.find("tcp dport 5277 accept"), std::string::npos);
      EXPECT_NE(boundary->rules.find("forward iifname \"wlan-test\" drop"), std::string::npos);
      EXPECT_NE(boundary->rules.find("forward oifname \"wlan-test\" drop"), std::string::npos);
      EXPECT_EQ(boundary->rules.find("dport 53 accept"), std::string::npos);
    }
    client->teardown(core::ConsumerId{1});
    ASSERT_TRUE(eventually([this] { return !nm->active_now && !boundary->table; }));
    EXPECT_GT(nm->deactivations, 0); EXPECT_GT(nm->deletions, 0);
    EXPECT_EQ(text, read_text(directory / "state/ap.credentials"));
    EXPECT_FALSE(client->call({Operation::open_pairing_window, 0}, core::Milliseconds{500}, {}));
}
TEST_F(NmDaemon, EffectiveCountryUnsetFailsBeforeExposure) {
    boundary->country = false; start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    EXPECT_EQ(nm->starts, 0); EXPECT_EQ(boundary->installations, 0);
}
TEST_F(NmDaemon, IneligibleChannelFailsBeforeExposure) {
    boundary->enabled = false; start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    EXPECT_EQ(nm->starts, 0); EXPECT_EQ(boundary->installations, 0);
}
TEST_F(NmDaemon, WrongChannelRollsBackWithout44Fallback) {
    boundary->wrong_channel = true; start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    ASSERT_TRUE(eventually([this] { return !nm->active_now && !boundary->table; })); EXPECT_EQ(nm->starts, 1);
}
TEST_F(NmDaemon, UnexpectedChannelSwitchEvictsActiveAp) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); boundary->wrong_channel = true;
    ASSERT_TRUE(state(State::unavailable)); ASSERT_TRUE(eventually([this] { return !boundary->table && !nm->active_now; }));
}
TEST_F(NmDaemon, FailedActivationRollsBackRules) {
    nm->deny = true; start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    EXPECT_EQ(nm->starts, 1); ASSERT_TRUE(eventually([this] { return !boundary->table; }));
}
TEST_F(NmDaemon, FirewallFailurePreventsNmActivation) {
    boundary->fail_install = true; start(); acquire(); ASSERT_TRUE(state(State::unavailable)); EXPECT_EQ(nm->starts, 0);
}
TEST_F(NmDaemon, LateReplyCannotOutliveBoundClient) {
    nm->late_reply = true; start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    ASSERT_TRUE(eventually([this] { return !nm->active_now && boundary->quarantined; }));
    EXPECT_TRUE(boundary->table); EXPECT_GT(nm->disconnects, 0);
}
TEST_F(NmDaemon, StartupDeadlineRejectsLateActivation) {
    nm->stall = true; start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    ASSERT_TRUE(eventually([this] { return !nm->pending && !boundary->table; }));
}
TEST_F(NmDaemon, CancelPendingActivationFencesPublication) {
    nm->stall = true; start(); acquire(); ASSERT_TRUE(eventually([this] { return nm->starts == 1; }));
    client->teardown(core::ConsumerId{1});
    ASSERT_TRUE(eventually([this] { return !nm->pending && !boundary->table; })); EXPECT_NE(client->status(Action::ap_status).value(), State::active);
}
TEST_F(NmDaemon, ReceiverLossCleansActualDaemonLease) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); client.reset();
    ASSERT_TRUE(eventually([this] { return !nm->active_now && !boundary->table; }, 5000ms));
}
TEST_F(NmDaemon, OwnerLossCannotUseReplacementInactiveAsShutdownProof) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); nm->stop(); ASSERT_TRUE(state(State::unavailable));
    EXPECT_TRUE(boundary->table);
    nm = std::make_unique<NetworkManager>(address, *boundary);
    std::this_thread::sleep_for(400ms);
    ASSERT_FALSE(nm->active_now); ASSERT_TRUE(boundary->table); EXPECT_TRUE(boundary->quarantined);
    const RevocationV1 snapshot{core::PhoneId{1}, trust::WirelessIdentity{"old-bond", "adapter"}};
    EXPECT_FALSE(revoke_ap(snapshot));
    nm->active_now = true;
    std::this_thread::sleep_for(400ms);
    EXPECT_TRUE(boundary->table); EXPECT_TRUE(boundary->quarantined);
    EXPECT_FALSE(revoke_ap(snapshot));
}
TEST_F(NmDaemon, FailedShutdownRetainsRulesAndRevocationRetries) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    const auto before = read_text(directory / "state/ap.credentials"); nm->fail_stop = true;
    const RevocationV1 snapshot{core::PhoneId{1}, trust::WirelessIdentity{"exact-bond", "adapter"}};
    EXPECT_FALSE(revoke_ap(snapshot)); EXPECT_TRUE(boundary->table);
    EXPECT_TRUE(boundary->quarantined);
    ASSERT_TRUE(eventually([&] { return read_text(directory / "state/ap.credentials") != before; }));
    const auto rotated = read_text(directory / "state/ap.credentials");
    nm->fail_stop = false; nm->active_now = false; nm->pending = false;
    ASSERT_TRUE(eventually([&] { return revoke_ap(snapshot).has_value(); }));
    EXPECT_FALSE(boundary->table); EXPECT_EQ(rotated, read_text(directory / "state/ap.credentials"));
}
TEST_F(NmDaemon, ForgetRotatesButOrdinaryInvalidateDoesNot) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    const auto before = read_text(directory / "state/ap.credentials");
    client->cancel(core::ConsumerId{1}, core::PairingRequestId{1});
    ASSERT_TRUE(eventually([this] { return !boundary->table; })); EXPECT_EQ(before, read_text(directory / "state/ap.credentials"));
    acquire(2); ASSERT_TRUE(state(State::active));
    const RevocationV1 snapshot{core::PhoneId{2}, trust::WirelessIdentity{"exact-bond", "adapter"}};
    ASSERT_TRUE(eventually([&] { return revoke_ap(snapshot).has_value(); }));
    EXPECT_FALSE(nm->active_now); EXPECT_FALSE(boundary->table); EXPECT_NE(before, read_text(directory / "state/ap.credentials"));
}
TEST_F(NmDaemon, StrictPolicyAndCredentialFilesystem) {
    const auto path = directory.string();
    EXPECT_FALSE(load_policy(path, 0));
    save(directory / "ap.conf", "v1 wlan-test US not-confirmed 5277\n"); EXPECT_FALSE(load_policy(path, ::getuid()));
    save(directory / "ap.conf", "v1 wlan-test US jurisdiction-confirmed 5277 extra\n"); EXPECT_FALSE(load_policy(path, ::getuid()));
    save(directory / "ap.conf", "v1 wlan-test US jurisdiction-confirmed 5277\n");
    const auto policy = load_policy(path, ::getuid()); ASSERT_TRUE(policy);
    const auto first = credentials(policy.value(), false); ASSERT_TRUE(first);
    const auto same = credentials(policy.value(), false); ASSERT_TRUE(same); EXPECT_EQ(first.value().psk, same.value().psk);
    const auto next = credentials(policy.value(), true); ASSERT_TRUE(next); EXPECT_NE(next.value().psk, first.value().psk);
    ::chmod((directory / "state/ap.credentials").c_str(), 0640); EXPECT_FALSE(credentials(policy.value(), false));
    std::filesystem::remove(directory / "state/ap.credentials");
    std::filesystem::create_symlink(directory / "ap.conf", directory / "state/ap.credentials");
    EXPECT_FALSE(credentials(policy.value(), true));
    ::chmod((directory / "ap.conf").c_str(), 0644); EXPECT_FALSE(load_policy(path, ::getuid()));
}
TEST_F(NmDaemon, CommittedForget703ReachesRealDaemonButPrecommitDoesNot) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    const auto before = read_text(directory / "state/ap.credentials");
    const trust::TransportIdentity identity{trust::WirelessIdentity{"old-bond", "exact-adapter"}};
    const auto phone = store->approve(identity); ASSERT_TRUE(phone);
    trust::SteadyClock clock;
    trust::PairingPolicy phones{*store, clock};
    ipc::test::FakePeerCredentials peers;
    ipc::test::RecordingEventSink events;
    ipc::test::SequencePairingIds ids;
    ipc::ControlService control{{::getuid()}, {peers, events, ids, phones, client.get()}};
    const ipc::BusName caller{":1.900"}, name{"org.custom.Consumer"};
    peers.add_connection(caller, ::getuid()); peers.set_owner(name, caller);
    ASSERT_TRUE(control.register_consumer(caller, name));
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::renameat};
        const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::trust_store_io);
    }
    EXPECT_TRUE(nm->active_now); EXPECT_EQ(before, read_text(directory / "state/ap.credentials"));
    {
        const trust::test::faults::ScopedFault fault{trust::test::faults::Fault::dir_fsync};
        const auto result = control.forget_phone(caller, phone.value()); ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code(), ErrorCode::trust_store_not_durable);
    }
    ASSERT_TRUE(eventually([&] { return !nm->active_now && !boundary->table && read_text(directory / "state/ap.credentials") != before; }));
    EXPECT_TRUE(store->phones().empty());
}
TEST_F(NmDaemon, SigkillRecoveryReconcilesOnlyOwnedTable) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    ::kill(daemon.pid, SIGKILL); ASSERT_EQ(::waitpid(daemon.pid, nullptr, 0), daemon.pid); daemon.pid = 0;
    ASSERT_TRUE(eventually([this] { return !nm->active_now; })); EXPECT_TRUE(boundary->table);
    start(); acquire(2); ASSERT_TRUE(state(State::active));
    EXPECT_EQ(boundary->installations, 2); EXPECT_GE(boundary->removals, 1);
}
TEST_F(NmDaemon, RegulatoryChangeRollsBackActiveAp) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); boundary->country = false;
    ASSERT_TRUE(state(State::unavailable)); ASSERT_TRUE(eventually([this] { return !nm->active_now && !boundary->table; }));
}
TEST_F(NmDaemon, UnconfirmedConfigurationAbortsDaemonBeforeEffects) {
    save(directory / "ap.conf", "v1 wlan-test US unconfirmed 5277\n");
    daemon.start({std::string{AA_HELPER_BUILD} + "/aa-system-helper", "--private-bus", address, directory.string(), (directory / "commands").string()});
    int status{};
    ASSERT_TRUE(eventually([&] { return ::waitpid(daemon.pid, &status, WNOHANG) == daemon.pid; })); daemon.pid = 0;
    ASSERT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status), 1); EXPECT_EQ(boundary->calls, 0); EXPECT_EQ(nm->starts, 0);
}
TEST_F(NmDaemon, PendingIsNotAppliedAndReplayCannotRestartAp) {
    nm->stall = true; start();
    ASSERT_TRUE(client->prepare(core::ConsumerId{1}, core::PairingRequestId{1}));
    ASSERT_TRUE(client->confirmed(core::ConsumerId{1}, core::PairingRequestId{1}));
    const auto response = client->call({Operation::ensure_access_point, 36}, core::Milliseconds{500}, {});
    ASSERT_TRUE(response); EXPECT_FALSE(response.value().applied);
    const auto polled = client->call({Operation::ensure_access_point, 36}, core::Milliseconds{500}, {});
    ASSERT_TRUE(polled); EXPECT_FALSE(polled.value().applied);
    EXPECT_FALSE(client->confirmed(core::ConsumerId{1}, core::PairingRequestId{1}));
    ASSERT_TRUE(eventually([this] { return !nm->pending && !boundary->table; }));
    EXPECT_LE(nm->starts, 1);
}
TEST_F(NmDaemon, PreexistingInfrastructureAndForeignRulesAreUntouched) {
    nm->active_now = true; boundary->table = true; boundary->foreign_table = true;
    start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    EXPECT_EQ(nm->starts, 0); EXPECT_EQ(nm->deactivations, 0); EXPECT_EQ(boundary->removals, 0);
    EXPECT_TRUE(nm->active_now); EXPECT_TRUE(boundary->table);
}
TEST_F(NmDaemon, ForeignTableAloneIsNeverRemoved) {
    boundary->table = true; boundary->foreign_table = true;
    start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    EXPECT_EQ(nm->starts, 0); EXPECT_EQ(boundary->removals, 0); EXPECT_TRUE(boundary->table);
}
TEST_F(NmDaemon, RuleRemovalFailureRemainsVisibleAndRetriesSafely) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); boundary->block_remove = true;
    client->teardown(core::ConsumerId{1});
    ASSERT_TRUE(eventually([this] { return !nm->active_now; }));
    EXPECT_TRUE(boundary->table); ASSERT_TRUE(state(State::unavailable));
    boundary->block_remove = false;
    ASSERT_TRUE(eventually([this] { return !boundary->table; })); ASSERT_TRUE(state(State::inactive));
}
TEST_F(NmDaemon, ActualDaemonRejectsOtherUniqueNameAndMalformedRevoke) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    const auto before = read_text(directory / "state/ap.credentials");
    auto* rogue = g_dbus_connection_new_for_address_sync(address.c_str(),
        static_cast<GDBusConnectionFlags>(G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION), nullptr, nullptr, nullptr);
    ASSERT_NE(rogue, nullptr);
    auto check = [&](const char* member, GVariant* args, unsigned expected) {
        auto* reply = g_dbus_connection_call_sync(rogue, kBusName, kObjectPath, kInterface, member, args,
            G_VARIANT_TYPE("(ut)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, nullptr);
        ASSERT_NE(reply, nullptr);
        unsigned error{}; guint64 value{}; g_variant_get(reply, "(ut)", &error, &value);
        EXPECT_EQ(error, expected); EXPECT_EQ(value, 0U); g_variant_unref(reply);
    };
    check("Execute", g_variant_new("(uut)", 4U, 0U, guint64{0}), 1001);
    check("RevokeAP1", g_variant_new("(t)", guint64{1}), 1001);
    check("RevokeAP1", g_variant_new("(s)", "bad-schema"), 1002);
    (void)g_dbus_connection_close_sync(rogue, nullptr, nullptr); g_object_unref(rogue);
    EXPECT_TRUE(nm->active_now); EXPECT_EQ(before, read_text(directory / "state/ap.credentials"));
}
TEST_F(NmDaemon, ExpiredGrantNeverReachesPlatformEffects) {
    start(); ASSERT_TRUE(client->prepare(core::ConsumerId{1}, core::PairingRequestId{1}));
    ASSERT_TRUE(client->confirmed(core::ConsumerId{1}, core::PairingRequestId{1}));
    std::this_thread::sleep_for(5100ms);
    const auto result = client->call({Operation::ensure_access_point, 36}, core::Milliseconds{500}, {});
    ASSERT_FALSE(result); EXPECT_EQ(result.error().code(), ErrorCode::helper_rejected);
    EXPECT_EQ(nm->starts, 0); EXPECT_EQ(boundary->installations, 0);
}
TEST_F(NmDaemon, FailedDeactivateCannotUseInactivePropertyAsAcknowledgement) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); nm->fail_stop = true;
    const RevocationV1 snapshot{core::PhoneId{1}, trust::WirelessIdentity{"old-bond", "adapter"}};
    nm->active_now = false; nm->pending = false;
    EXPECT_FALSE(revoke_ap(snapshot));
    EXPECT_TRUE(boundary->table); EXPECT_TRUE(boundary->quarantined);
    nm->active_now = true;
    EXPECT_FALSE(revoke_ap(snapshot)); EXPECT_TRUE(boundary->table);
    nm->fail_stop = false;
    ASSERT_TRUE(eventually([&] { return revoke_ap(snapshot).has_value(); }));
    EXPECT_FALSE(nm->active_now); EXPECT_FALSE(boundary->table);
}
TEST_F(NmDaemon, FirewallEnumerationErrorIsNotAbsence) {
    boundary->fail_listing = true; start(); acquire(); ASSERT_TRUE(state(State::unavailable));
    EXPECT_EQ(nm->starts, 0); EXPECT_EQ(boundary->installations, 0);
}
TEST_F(NmDaemon, BusLossRetainsQuarantineWhenShutdownCannotBeProved) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); bus.stop();
    int status{};
    ASSERT_TRUE(eventually([&] { return ::waitpid(daemon.pid, &status, WNOHANG) == daemon.pid; })); daemon.pid = 0;
    ASSERT_TRUE(WIFEXITED(status)); EXPECT_EQ(WEXITSTATUS(status), 1);
    EXPECT_TRUE(boundary->table); EXPECT_TRUE(boundary->quarantined);
}
TEST_F(NmDaemon, GracefulDaemonShutdownRemovesOwnedResources) {
    start(); acquire(); ASSERT_TRUE(state(State::active)); daemon.stop();
    EXPECT_FALSE(nm->active_now); EXPECT_FALSE(boundary->table); EXPECT_GT(nm->deletions, 0);
}
struct DoubleEndpoints { std::string bus, command_socket; };
int wire_double(const DoubleEndpoints& endpoints) {
    Boundary boundary{endpoints.command_socket};
    NetworkManager nm{endpoints.bus, boundary};
    std::cout << "ready\n" << std::flush;
    for (std::string command; std::getline(std::cin, command);) {
        if (command == "state") {
            std::cout << std::boolalpha << "{\"active\":" << nm.active_now.load()
                << ",\"valid\":" << nm.valid.load() << ",\"starts\":" << nm.starts.load()
                << ",\"deactivations\":" << nm.deactivations.load()
                << ",\"table\":" << boundary.table.load() << ",\"quarantined\":" << boundary.quarantined.load() << "}\n";
        } else {
            if (command == "fail-stop") nm.fail_stop = true;
            else if (command == "allow-stop") nm.fail_stop = false;
            else if (command == "block-remove") boundary.block_remove = true;
            else if (command == "allow-remove") boundary.block_remove = false;
            else return 2;
            std::cout << "ok\n";
        }
        std::cout << std::flush;
    }
    return ::testing::Test::HasFailure() ? 1 : 0;
}
}
}
int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    if (argc == 4 && std::string{argv[1]} == "--wire-double")
        return aa::helper::nm::wire_double({.bus = argv[2], .command_socket = argv[3]});
    return RUN_ALL_TESTS();
}
