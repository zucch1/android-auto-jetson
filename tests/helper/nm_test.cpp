// SPDX-License-Identifier: GPL-3.0-or-later
#include "../../src/helper/nm/Platform.hpp"
#include "../../src/helper/crypto_adapter/Crypto.hpp"
#include <aa/helper/BusClient.hpp>
#include <aa/helper/BusService.hpp>
#include <aa/helper/AawFrame.hpp>
#include <aa/helper/BluezBackend.hpp>
#include <aa/helper/BackendRouter.hpp>
#include "Bootstrap.pb.h"
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
    int output{-1};
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
        output = fd[0]; ASSERT_EQ(ready, "ready");
    }
    std::string command(const std::string& text) {
        const auto bytes = text + "\n";
        if (::write(input, bytes.data(), bytes.size()) != static_cast<ssize_t>(bytes.size())) return "write-failed";
        ::pollfd event{output, POLLIN, 0}; std::string result; char byte{};
        while (::poll(&event, 1, 4000) == 1 && ::read(output, &byte, 1) == 1 && byte != '\n') result += byte;
        return result;
    }
    void stop() {
        if (pid) { ::kill(pid, SIGTERM); EXPECT_TRUE(eventually([this] { return ::waitpid(pid, nullptr, WNOHANG) == pid; }, 8000ms)); pid = 0; }
        if (input >= 0) { ::close(input); input = -1; }
        if (output >= 0) { ::close(output); output = -1; }
    }
    ~Child() { stop(); }
};
class Boundary final {
public:
    std::atomic<bool> table{}, country{true}, enabled{true}, wrong_channel{}, block_remove{}, fail_install{}, foreign_table{}, quarantined{}, fail_listing{};
    std::atomic<int> installations{}, removals{}, radios{}, calls{};
    std::atomic<unsigned> channel{36};
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
                        std::string{(channel == 36) == static_cast<bool>(wrong_channel) ? "44 (5220 MHz)" : "36 (5180 MHz)"} +
                        ", width: 20 MHz\n";
                } else if (request == "/usr/sbin/iw\ndev\n\n") reply = "0phy#0\n\tInterface wlan-test\n";
                else if (request == "/usr/sbin/iw\nreg\nget\n\n") reply = country ? "0global\ncountry US: DFS-FCC\n" : "0global\ncountry 00: DFS-UNSET\n";
                else if (request == "/usr/sbin/iw\nphy\nphy0\ninfo\n\n") reply = enabled ? "0 * 5180.0 MHz [36] (23.0 dBm)\n * 5220.0 MHz [44] (23.0 dBm)\n" :
                                                                                "0 * 5180.0 MHz [36] (disabled)\n * 5220.0 MHz [44] (disabled)\n";
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
                } else if (request.starts_with("/usr/sbin/nft\n-f\n-\n\nflush chain inet aa_receiver_ap input\n")) {
                    if (table) { const std::lock_guard lock{mutex}; rules = request; quarantined = true; reply = "0"; }
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
<property name="DeviceType" type="u" access="read"/><property name="Interface" type="s" access="read"/><property name="ActiveConnection" type="o" access="read"/><property name="Ip4Config" type="o" access="read"/>
</interface>
<interface name="org.freedesktop.NetworkManager.Connection.Active"><property name="State" type="u" access="read"/></interface>
<interface name="org.freedesktop.NetworkManager.Settings.Connection"><method name="Delete"/></interface>
<interface name="org.freedesktop.NetworkManager.Device.Wireless"><property name="Mode" type="u" access="read"/><property name="HwAddress" type="s" access="read"/></interface>
<interface name="org.freedesktop.NetworkManager.IP4Config"><property name="AddressData" type="aa{sv}" access="read"/></interface>
</node>)xml";
class NetworkManager final {
public:
    std::atomic<bool> active_now{}, pending{}, deny{}, stall{}, fail_stop{}, late_reply{}, valid{true};
    std::atomic<int> starts{}, deactivations{}, deletions{}, disconnects{};
    std::atomic<Clock::duration::rep> teardown_started{};
    Boundary& boundary;
    std::mutex mutex;
    std::string seen_psk;
    std::atomic<bool> bad_mac{}, replaced{}, wrong_mode{}, changed_mac{}, changed_ip{};
    std::atomic<int> release_owner_at_address{}, replace_device_at_address{}, change_channel_at_address{}, address_reads{}, delay_address_ms{};
    std::atomic<int> deactivate_delay_ms{}, client_stop_delay_ms{};
    std::atomic<unsigned> expected_channel{36};
    std::atomic<bool> owner_released{}, device_remapped{};
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
            EXPECT_NE(g_dbus_connection_register_object(bus_, dev, node->interfaces[4], &vtable, this, nullptr, nullptr), 0U);
            EXPECT_NE(g_dbus_connection_register_object(bus_, "/org/freedesktop/NetworkManager/IP4Config/1", node->interfaces[5], &vtable, this, nullptr, nullptr), 0U);
            const auto subscription = g_dbus_connection_signal_subscribe(bus_, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged",
                "/org/freedesktop/DBus", nullptr, G_DBUS_SIGNAL_FLAGS_NONE, owner_changed, this, nullptr);
            ready.set_value(true);
            while (!stop.stop_requested()) {
                advance_deactivation();
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
    std::atomic<bool> deactivating{};
    std::atomic<int> deactivation_starts{};
    std::atomic<Clock::duration::rep> deactivation_due{};
    void schedule_deactivation(int delay) {
        if (delay > 0) {
            deactivation_starts = starts.load();
            deactivation_due = (Clock::now() + std::chrono::milliseconds{delay}).time_since_epoch().count();
            deactivating = true;
            return;
        }
        deactivation_due = 0; deactivating = false; active_now = false; pending = false;
    }
    void advance_deactivation() {
        const auto due = deactivation_due.load();
        if (!deactivating || !due || Clock::now().time_since_epoch().count() < due) return;
        if (starts.load() == deactivation_starts.load()) { active_now = false; pending = false; }
        deactivating = false; deactivation_due = 0;
    }
    static void owner_changed(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* args, gpointer data) {
        auto& self = *static_cast<NetworkManager*>(data);
        const char *name{}, *old{}, *next{}; g_variant_get(args, "(&s&s&s)", &name, &old, &next);
        if (self.client_ == name && *old && !*next && !self.fail_stop) {
            ++self.disconnects; self.schedule_deactivation(self.client_stop_delay_ms.load());
        }
    }
    static GVariant* property(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* name, GError**, gpointer data) {
        auto& self = *static_cast<NetworkManager*>(data);
        if (std::string{name} == "DeviceType") return g_variant_new_uint32(2);
        if (std::string{name} == "Interface") return g_variant_new_string(self.replaced ? "replacement" : "wlan-test");
        if (std::string{name} == "Mode") return g_variant_new_uint32(self.wrong_mode ? 2 : 3);
        if (std::string{name} == "HwAddress") return g_variant_new_string(self.bad_mac ? "bad" : self.changed_mac ? "02:AB:CD:EF:01:24" : "02:AB:CD:EF:01:23");
        if (std::string{name} == "Ip4Config") return g_variant_new_object_path("/org/freedesktop/NetworkManager/IP4Config/1");
        if (std::string{name} == "AddressData") {
            const auto read = ++self.address_reads;
            if (self.release_owner_at_address == read) {
                auto* reply = g_dbus_connection_call_sync(self.bus_, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                    "org.freedesktop.DBus", "ReleaseName", g_variant_new("(s)", manager), G_VARIANT_TYPE("(u)"),
                    G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
                self.owner_released = reply != nullptr;
                if (reply) g_variant_unref(reply);
            }
            if (self.replace_device_at_address == read) self.device_remapped = true;
            if (self.change_channel_at_address == read) self.boundary.wrong_channel = true;
            std::this_thread::sleep_for(std::chrono::milliseconds{self.delay_address_ms.load()});
            GVariantBuilder entries, entry; g_variant_builder_init(&entries, G_VARIANT_TYPE("aa{sv}"));
            g_variant_builder_init(&entry, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&entry, "{sv}", "address", g_variant_new_string(self.changed_ip ? "10.42.0.2" :
#ifdef AA_NM_TEST_ENDPOINT
                AA_NM_TEST_ENDPOINT
#else
                "10.42.0.1"
#endif
            ));
            g_variant_builder_add(&entry, "{sv}", "prefix", g_variant_new_uint32(24));
            g_variant_builder_add_value(&entries, g_variant_builder_end(&entry)); return g_variant_builder_end(&entries);
        }
        if (std::string{name} == "ActiveConnection") return g_variant_new_object_path(self.active_now || self.pending || self.deactivating ? active : "/");
        if (std::string{name} == "State") return g_variant_new_uint32(self.deactivating ? 3 : self.active_now ? 2 : self.pending ? 1 : 4);
        return nullptr;
    }
    static void method(GDBusConnection*, const gchar* sender, const gchar*, const gchar*, const gchar* name,
                       GVariant* args, GDBusMethodInvocation* invocation, gpointer data) {
        auto& self = *static_cast<NetworkManager*>(data);
        const std::string member{name};
        if (member == "GetDeviceByIpIface") { g_dbus_method_invocation_return_value(invocation,
            g_variant_new("(o)", self.device_remapped ? "/org/freedesktop/NetworkManager/Devices/2" : dev)); return; }
        if (member == "DeactivateConnection") {
            self.teardown_started = Clock::now().time_since_epoch().count();
            ++self.deactivations;
            if (self.fail_stop) { g_dbus_method_invocation_return_dbus_error(invocation, "org.example.Failed", "blocked"); return; }
            self.schedule_deactivation(self.deactivate_delay_ms.load());
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
                g_variant_lookup(wireless, "channel", "u", &channel) && channel == self.expected_channel &&
                g_variant_lookup(security, "key-mgmt", "&s", &key) && std::string{key} == "wpa-psk" &&
                g_variant_lookup(security, "psk", "&s", &psk) && std::string{psk}.size() == 64 &&
                g_variant_lookup(ipv4, "method", "&s", &ipv4_method) && std::string{ipv4_method} == "shared";
            if (self.valid) { const std::lock_guard lock{self.mutex}; self.seen_psk = psk; }
            if (wireless) g_variant_unref(wireless);
            if (security) g_variant_unref(security);
            if (ipv4) g_variant_unref(ipv4);
            g_variant_unref(settings); g_variant_unref(options);
            if (self.deny) { g_dbus_method_invocation_return_dbus_error(invocation, "org.example.Denied", "denied"); return; }
            self.deactivating = false; self.deactivation_due = 0;
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
    // Per-activation credentials stay in memory; no secret is written at rest.
    EXPECT_FALSE(std::filesystem::exists(directory / "state/ap.credentials"));
    { const std::lock_guard lock{boundary->mutex};
      EXPECT_NE(boundary->rules.find("udp sport 68 udp dport 67 accept"), std::string::npos);
      EXPECT_NE(boundary->rules.find("tcp dport 5277 accept"), std::string::npos);
      const auto ingress = boundary->rules.find("input iifname != \"wlan-test\" tcp dport 5277 drop");
      EXPECT_NE(ingress, std::string::npos);
      EXPECT_LT(ingress, boundary->rules.find("tcp dport 5277 accept"));
      EXPECT_NE(boundary->rules.find("forward iifname \"wlan-test\" drop"), std::string::npos);
      EXPECT_NE(boundary->rules.find("forward oifname \"wlan-test\" drop"), std::string::npos);
      EXPECT_EQ(boundary->rules.find("dport 53 accept"), std::string::npos);
    }
    client->teardown(core::ConsumerId{1});
    ASSERT_TRUE(eventually([this] { return !nm->active_now && !boundary->table; }));
    EXPECT_GT(nm->deactivations, 0); EXPECT_GT(nm->deletions, 0);
    EXPECT_FALSE(std::filesystem::exists(directory / "state/ap.credentials"));
    EXPECT_FALSE(client->call({Operation::open_pairing_window, 0}, core::Milliseconds{500}, {}));
}
TEST_F(NmDaemon, ActivationSnapshotUsesActivationCredentialsAndCurrentLocalMac) {
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    auto snapshot = backend.ap_snapshot(); ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot.value().bssid, "02:ab:cd:ef:01:23"); EXPECT_EQ(snapshot.value().ip, "10.42.0.1"); EXPECT_EQ(snapshot.value().port, 5277U);
    { const std::lock_guard lock{nm->mutex}; EXPECT_TRUE(snapshot.value().password == nm->seen_psk); }
    EXPECT_EQ(snapshot.value().password.size(), 64U);
    auto cached = backend.ap_snapshot(); ASSERT_TRUE(cached); EXPECT_TRUE(cached.value() == snapshot.value());
    EXPECT_TRUE(backend.snapshot_current(snapshot.value().generation));
    backend.cleanup(); EXPECT_FALSE(backend.snapshot_current(snapshot.value().generation)); EXPECT_FALSE(backend.ap_snapshot());
}
TEST_F(NmDaemon, EveryActivationAndRekeyIssuesFreshCredentials) {
    // Given an active AP whose credentials were released for generation 1.
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    const auto fresh = [&](std::uint64_t old) -> std::optional<ApSnapshot> {
        std::optional<ApSnapshot> result;
        EXPECT_TRUE(eventually([&] {
            auto snapshot = backend.ap_snapshot();
            if (!snapshot || snapshot.value().generation == old) return false;
            result = snapshot.value(); return true;
        }, 10s));
        return result;
    };
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    const auto first = fresh(0); ASSERT_TRUE(first);
    // When a rebind rekeys it, and later a new activation starts.
    backend.rekey(first->generation);
    const auto rekeyed = fresh(first->generation); ASSERT_TRUE(rekeyed);
    ASSERT_TRUE(backend.apply(Action::release_ap, 0));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::inactive; }));
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    const auto next = fresh(rekeyed->generation); ASSERT_TRUE(next);
    // Then no earlier SSID/PSK admits a station to the newer activation.
    EXPECT_FALSE(backend.snapshot_current(first->generation));
    EXPECT_NE(rekeyed->ssid, first->ssid); EXPECT_FALSE(rekeyed->password == first->password);
    EXPECT_NE(next->ssid, rekeyed->ssid); EXPECT_FALSE(next->password == rekeyed->password);
    EXPECT_EQ(nm->starts, 3);
}
TEST_F(NmDaemon, DelayedDeactivationDoesNotConsumeRekey) {
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    const auto first = backend.ap_snapshot(); ASSERT_TRUE(first);
    nm->deactivate_delay_ms = 300;
    backend.rekey(first.value().generation);
    EXPECT_FALSE(backend.snapshot_current(first.value().generation));
    ASSERT_TRUE(eventually([&] { return nm->deactivations > 0; }));
    EXPECT_TRUE(boundary->quarantined);
    { const std::lock_guard lock{boundary->mutex};
      EXPECT_NE(boundary->rules.find("input tcp dport 5277 drop"), std::string::npos);
      EXPECT_NE(boundary->rules.find("input iifname \"wlan-test\" drop"), std::string::npos); }
    std::optional<ApSnapshot> replacement;
    ASSERT_TRUE(eventually([&] {
        auto snapshot = backend.ap_snapshot();
        if (!snapshot || snapshot.value().generation == first.value().generation) return false;
        replacement = snapshot.value(); return true;
    }, 10s));
    ASSERT_TRUE(replacement);
    EXPECT_NE(replacement->ssid, first.value().ssid); EXPECT_NE(replacement->password, first.value().password);
    EXPECT_EQ(nm->starts, 2);
    nm->deactivate_delay_ms = 0;
}
TEST_F(NmDaemon, SnapshotRejectsMalformedMacModeAndDeviceChanges) {
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    nm->bad_mac = true; EXPECT_FALSE(backend.ap_snapshot()); nm->bad_mac = false;
    nm->wrong_mode = true; EXPECT_FALSE(backend.ap_snapshot()); nm->wrong_mode = false;
    nm->replaced = true; EXPECT_FALSE(backend.ap_snapshot()); nm->replaced = false;
    boundary->wrong_channel = true; EXPECT_FALSE(backend.ap_snapshot());
}
TEST_F(NmDaemon, ConfiguredChannelBuildsAndVerifiesAp) {
    // Given an operator policy for channel 44 and a radio that honours it.
    boundary->channel = 44; nm->expected_channel = 44;
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid(), 44}, address,
        private_commands((directory / "commands").string())};
    // When the receiver requests another channel, then the helper refuses it.
    EXPECT_FALSE(backend.apply(Action::acquire_ap, 36));
    // When it requests the configured channel, then NM builds and iw verifies that channel.
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 44));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    EXPECT_TRUE(nm->valid); ASSERT_TRUE(backend.ap_snapshot());
    // When the driver moves the AP off the configured channel, then no snapshot is released.
    boundary->wrong_channel = true; EXPECT_FALSE(backend.ap_snapshot());
}
TEST_F(NmDaemon, OwnerReleaseDuringFinalEndpointReadCannotPublishOldUniqueReply) {
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    nm->release_owner_at_address = 2;
    const auto snapshot = backend.ap_snapshot();
    EXPECT_TRUE(nm->owner_released); EXPECT_EQ(nm->address_reads, 2);
    EXPECT_FALSE(snapshot);
}
TEST_F(NmDaemon, DeviceRemapDuringFinalEndpointReadCannotPublishCachedGeneration) {
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    nm->replace_device_at_address = 2;
    EXPECT_FALSE(backend.ap_snapshot()); EXPECT_TRUE(nm->device_remapped);
}
TEST_F(NmDaemon, DeviceReplacementAfterSnapshotCannotPassTransmitFence) {
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    const auto snapshot = backend.ap_snapshot(); ASSERT_TRUE(snapshot);
    nm->device_remapped = true; bool transmitted{};
    EXPECT_FALSE(backend.with_snapshot(snapshot.value().generation, [&] { transmitted = true; }));
    EXPECT_FALSE(transmitted);
}
TEST_F(NmDaemon, ChannelChangeDuringTransmitFenceCannotSendCredentialsOrReviveEpoch) {
    // Given a collected snapshot on channel36, change only the radio at the next fence read.
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    const auto snapshot = backend.ap_snapshot(); ASSERT_TRUE(snapshot);
    nm->change_channel_at_address = nm->address_reads + 1;
    bool transmitted{};
    // When transmission is requested, the endpoint read changes the channel to44.
    const auto sent = backend.with_snapshot(snapshot.value().generation, [&] { transmitted = true; });
    // Then no credential callback runs, and restoring the radio cannot revive authority.
    EXPECT_TRUE(boundary->wrong_channel); EXPECT_FALSE(nm->device_remapped);
    EXPECT_FALSE(sent); EXPECT_FALSE(transmitted);
    boundary->wrong_channel = false;
    EXPECT_FALSE(backend.snapshot_current(snapshot.value().generation));
}
TEST_F(NmDaemon, OwnerReleaseDuringTransmitFenceCannotUsePreviouslyValidatedSnapshot) {
    Backend backend{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    ASSERT_TRUE(backend.apply(Action::acquire_ap, 36));
    ASSERT_TRUE(eventually([&] { return backend.apply(Action::ap_status, 0).value() == State::active; }));
    const auto snapshot = backend.ap_snapshot(); ASSERT_TRUE(snapshot);
    nm->release_owner_at_address = nm->address_reads + 1; bool transmitted{};
    EXPECT_FALSE(backend.with_snapshot(snapshot.value().generation, [&] { transmitted = true; }));
    EXPECT_TRUE(nm->owner_released); EXPECT_FALSE(transmitted);
    EXPECT_FALSE(backend.snapshot_current(snapshot.value().generation));
}
std::vector<std::uint8_t> unhex(const std::string& hex) {
    std::vector<std::uint8_t> bytes;
    if (hex == "open" || hex == "closed" || hex.size() % 2) return bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return bytes;
}
TEST_F(NmDaemon, ComposedBootstrapUsesRealBusClientAndRfcommBytes) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    ASSERT_EQ(bluez.command("paired"), "ok"); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    HandoffFailure failure{}; auto peer = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(peer);
    const auto handle = peer.value().handle;
    EXPECT_FALSE(client->get_ap_credentials({2, 2}, handle)); EXPECT_FALSE(client->get_ap_credentials({1, 1}, handle + 1));
    auto rogue = BusClient::private_bus(address, *store); ASSERT_TRUE(rogue);
    EXPECT_FALSE(rogue.value()->get_ap_credentials({1, 1}, handle));
    auto snapshot = client->get_ap_credentials({1, 1}, handle); ASSERT_TRUE(snapshot);
    EXPECT_EQ(bluez.command("wire-read"), "open");
    ASSERT_TRUE(client->authorize_bootstrap({1, 1}, handle, true, true));
    AawDecoder start_decoder; AawDecodeResult first;
    ASSERT_TRUE(eventually([&] {
        start_decoder.feed(unhex(bluez.command("wire-read"))); first = start_decoder.next();
        return !std::holds_alternative<std::monostate>(first);
    }));
    ASSERT_TRUE(std::holds_alternative<AawRecord>(first));
    EXPECT_EQ(std::get<AawRecord>(first).id, 4); aaw::WifiVersionRequest version;
    ASSERT_TRUE(version.ParseFromArray(std::get<AawRecord>(first).payload.data(), static_cast<int>(std::get<AawRecord>(first).payload.size())));
    EXPECT_EQ(version.major_version(), 1); EXPECT_EQ(version.minor_version(), 0);
    EXPECT_EQ(version.selected_channel(), aaw::CHANNEL_5GHZ_ONLY);
    ASSERT_EQ(bluez.command("wire-send 00000002"), "ok");
    AawDecoder info_decoder; AawDecodeResult info_record;
    ASSERT_TRUE(eventually([&] {
        info_decoder.feed(unhex(bluez.command("wire-read"))); info_record = info_decoder.next();
        return !std::holds_alternative<std::monostate>(info_record);
    }));
    ASSERT_TRUE(std::holds_alternative<AawRecord>(info_record)); EXPECT_EQ(std::get<AawRecord>(info_record).id, 3);
    aaw::WifiInfoResponse info; ASSERT_TRUE(info.ParseFromArray(std::get<AawRecord>(info_record).payload.data(), static_cast<int>(std::get<AawRecord>(info_record).payload.size())));
    EXPECT_TRUE(info.password() == snapshot.value().password); EXPECT_EQ(info.bssid(), "02:ab:cd:ef:01:23");
    EXPECT_EQ(info.security_mode(), 8); EXPECT_EQ(info.access_point_type(), 1);
    AawDecodeResult endpoint_record;
    ASSERT_TRUE(eventually([&] {
        endpoint_record = info_decoder.next();
        if (!std::holds_alternative<std::monostate>(endpoint_record)) return true;
        info_decoder.feed(unhex(bluez.command("wire-read"))); endpoint_record = info_decoder.next();
        return !std::holds_alternative<std::monostate>(endpoint_record);
    }));
    ASSERT_TRUE(std::holds_alternative<AawRecord>(endpoint_record)); EXPECT_EQ(std::get<AawRecord>(endpoint_record).id, 1);
    aaw::WifiStartRequest endpoint;
    ASSERT_TRUE(endpoint.ParseFromArray(std::get<AawRecord>(endpoint_record).payload.data(), static_cast<int>(std::get<AawRecord>(endpoint_record).payload.size())));
    EXPECT_EQ(endpoint.ip_address(), "10.42.0.1"); EXPECT_EQ(endpoint.port(), 5277U);
    ASSERT_EQ(bluez.command("wire-send 000200071800000200060800"), "ok");
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    // Contract v2.4: successful FD closure transfers revocable authority, not retirement.
    const auto completed = client->get_ap_credentials({1, 1}, handle); ASSERT_TRUE(completed);
    EXPECT_TRUE(completed.value() == snapshot.value());
    const auto polled = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(polled);
    EXPECT_EQ(polled.value().handle, handle);
    EXPECT_FALSE(client->get_ap_credentials({2, 2}, handle));
    EXPECT_FALSE(client->get_ap_credentials({1, 1}, handle + 1));
    EXPECT_FALSE(rogue.value()->get_ap_credentials({1, 1}, handle));
}
#include "nm_lifecycle_tests.inc"
TEST_F(NmDaemon, ComposedReceiverDenialClosesWithoutAnyPayload) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    ASSERT_EQ(bluez.command("paired"), "ok"); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    HandoffFailure failure{}; auto peer = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(peer);
    ASSERT_TRUE(client->authorize_bootstrap({1, 1}, peer.value().handle, false, false));
    EXPECT_EQ(bluez.command("wire-read"), "closed"); EXPECT_FALSE(client->get_ap_credentials({1, 1}, peer.value().handle));
    EXPECT_FALSE(client->poll_bootstrap({1, 1}, failure)); EXPECT_EQ(failure, HandoffFailure::denied);
}
TEST_F(NmDaemon, ComposedReplacementInSameAdmissionNeedsFreshHandleAuthorization) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    ASSERT_EQ(bluez.command("paired"), "ok"); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    HandoffFailure failure{}; const auto old = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(old);
    ASSERT_TRUE(client->get_ap_credentials({1, 1}, old.value().handle));
    ASSERT_TRUE(client->authorize_bootstrap({1, 1}, old.value().handle, false, false));
    ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    const auto fresh = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(fresh);
    EXPECT_NE(fresh.value().handle, old.value().handle); EXPECT_EQ(bluez.command("wire-read"), "open");
    EXPECT_FALSE(client->get_ap_credentials({1, 1}, old.value().handle));
    ASSERT_TRUE(client->get_ap_credentials({1, 1}, fresh.value().handle));
    ASSERT_TRUE(client->authorize_bootstrap({1, 1}, fresh.value().handle, true, true));
    AawDecoder decoder; AawDecodeResult endpoint;
    ASSERT_TRUE(eventually([&] {
        decoder.feed(unhex(bluez.command("wire-read"))); endpoint = decoder.next();
        return !std::holds_alternative<std::monostate>(endpoint);
    }));
    ASSERT_TRUE(std::holds_alternative<AawRecord>(endpoint)); EXPECT_EQ(std::get<AawRecord>(endpoint).id, 4);
    ASSERT_EQ(bluez.command("RequestDisconnection"), "allowed");
    ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    const auto third = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(third);
    EXPECT_NE(third.value().handle, fresh.value().handle); EXPECT_EQ(bluez.command("wire-read"), "open");
    EXPECT_FALSE(client->get_ap_credentials({1, 1}, fresh.value().handle));
    EXPECT_TRUE(client->get_ap_credentials({1, 1}, third.value().handle));
    EXPECT_TRUE(client->authorize_bootstrap({1, 1}, third.value().handle, true, true));
}
TEST_F(NmDaemon, ExpiredHandoffRejectsOnUseWithoutPlatformTick) {
    const auto began = Clock::now(); std::atomic<std::int64_t> elapsed{};
    BluezBackend bluetooth{address, [&] { return began + std::chrono::milliseconds{elapsed.load()}; }};
    ASSERT_TRUE(bluetooth.apply(Action::register_profile, 0));
    ASSERT_EQ(bluez.command("paired"), "ok"); const auto observed = bluetooth.observe(); ASSERT_TRUE(observed);
    ASSERT_TRUE(bluetooth.reconnect_peer(observed.value().handle));
    auto incoming = std::async(std::launch::async, [&] { return bluez.command("NewConnection"); });
    ASSERT_TRUE(eventually([&] { bluetooth.tick(); return incoming.wait_for(0ms) == std::future_status::ready; }));
    ASSERT_EQ(incoming.get(), "allowed"); ASSERT_TRUE(bluetooth.handoff());
    elapsed = 10000;
    EXPECT_FALSE(bluetooth.handoff()); EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_EQ(bluetooth.handoff_failure(), HandoffFailure::timeout);
}
TEST_F(NmDaemon, NearExpiryDelayedCollectionClosesSocketAndReturnsEmptyTypedError) {
    const auto began = Clock::now();
    std::atomic<std::int64_t> offset{}, resumed_at{};
    BluezBackend bluetooth{address, [&] {
        const auto resumed = resumed_at.load();
        return began + std::chrono::milliseconds{offset.load()} + (resumed ? Clock::now().time_since_epoch() - Clock::duration{resumed} : Clock::duration{});
    }};
    Backend ap{Policy{"wlan-test", "US", 5277, (directory / "state").string(), ::getuid()}, address,
        private_commands((directory / "commands").string())};
    BackendRouter router{{ap, bluetooth}};
    const int fd = ::open("/proc/self/exe", O_RDONLY | O_CLOEXEC); ASSERT_GE(fd, 0);
    const auto digest = crypto_adapter::hash_executable(fd, Clock::now() + 1s); ::close(fd); ASSERT_TRUE(digest);
    auto service = BusService::private_bus(address, {::getuid(), digest.value()}, router); ASSERT_TRUE(service);
    std::jthread pump{[&](const std::stop_token& stop) { while (!stop.stop_requested()) if (!service.value().cycle()) return; }};
    auto connected = BusClient::private_bus(address, *store); ASSERT_TRUE(connected); client = std::move(connected.value());
    acquire(); ASSERT_TRUE(state(State::active));
    ASSERT_EQ(bluez.command("paired"), "ok"); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    HandoffFailure failure{}; const auto peer = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(peer);
    nm->delay_address_ms = 120; offset = 9850;
    auto fetched = std::async(std::launch::async, [&] { return client->get_ap_credentials({1, 1}, peer.value().handle); });
    ASSERT_TRUE(eventually([&] { return nm->address_reads > 0; }, 1000ms));
    resumed_at = Clock::now().time_since_epoch().count();
    const auto closure = bluez.command("wire-read");
    EXPECT_EQ(closure, "closed");
    const auto result = fetched.get(); EXPECT_FALSE(result);
    if (!result) { EXPECT_EQ(result.error().code(), ErrorCode::helper_rejected); }
    pump.request_stop(); pump.join();
}
TEST_F(NmDaemon, ComposedApprovedReconnectFetchesWithoutPendingAddPhone) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    const trust::TransportIdentity identity{trust::WirelessIdentity{"02:00:00:00:00:01", "02:00:00:00:00:AA"}};
    ASSERT_TRUE(store->approve(identity)); ASSERT_EQ(bluez.command("paired"), "ok");
    ASSERT_TRUE(client->reconnect(identity)); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    HandoffFailure failure{}; const auto peer = client->poll_bootstrap({7, 0}, failure); ASSERT_TRUE(peer);
    ASSERT_TRUE(client->get_ap_credentials({7, 0}, peer.value().handle));
    EXPECT_EQ(store->phones().size(), 1U); EXPECT_FALSE(client->get_ap_credentials({1, 1}, peer.value().handle));
    EXPECT_TRUE(client->authorize_bootstrap({7, 0}, peer.value().handle, false, false));
}
TEST_F(NmDaemon, ComposedBssidChangeAfterFetchFailsBeforeEndpoint) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    ASSERT_EQ(bluez.command("paired"), "ok"); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    HandoffFailure failure{}; const auto peer = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(peer);
    ASSERT_TRUE(client->get_ap_credentials({1, 1}, peer.value().handle)); nm->changed_mac = true;
    EXPECT_FALSE(client->authorize_bootstrap({1, 1}, peer.value().handle, true, true));
    EXPECT_EQ(bluez.command("wire-read"), "closed");
    EXPECT_FALSE(client->poll_bootstrap({1, 1}, failure)); EXPECT_EQ(failure, HandoffFailure::snapshot);
}
TEST_F(NmDaemon, AcceptedBootstrapOutlivesAdmissionButNotItsOwnTenSeconds) {
    const auto began = Clock::now(); std::atomic<std::int64_t> elapsed{};
    BluezBackend bluetooth{address, [&] { return began + std::chrono::milliseconds{elapsed.load()}; }};
    ASSERT_TRUE(bluetooth.apply(Action::register_profile, 0));
    ASSERT_EQ(bluez.command("paired"), "ok"); const auto observed = bluetooth.observe(); ASSERT_TRUE(observed);
    ASSERT_TRUE(bluetooth.reconnect_peer(observed.value().handle));
    elapsed = 59000;
    auto incoming = std::async(std::launch::async, [&] { return bluez.command("NewConnection"); });
    ASSERT_TRUE(eventually([&] { bluetooth.tick(); return incoming.wait_for(0ms) == std::future_status::ready; }));
    ASSERT_EQ(incoming.get(), "allowed"); const auto handoff = bluetooth.handoff(); ASSERT_TRUE(handoff);
    elapsed += 2000; bluetooth.tick(); EXPECT_TRUE(bluetooth.handoff());
    auto late = std::async(std::launch::async, [&] { return bluez.command("NewConnection"); });
    ASSERT_TRUE(eventually([&] { bluetooth.tick(); return late.wait_for(0ms) == std::future_status::ready; }));
    EXPECT_EQ(late.get(), "rejected"); EXPECT_TRUE(bluetooth.handoff());
    elapsed += 8000; bluetooth.tick(); EXPECT_FALSE(bluetooth.handoff()); EXPECT_EQ(bluetooth.handoff_failure(), HandoffFailure::timeout);
}
TEST_F(NmDaemon, ComposedRevocationAfterSnapshotPreventsCredentialSend) {
    start(); acquire(); ASSERT_TRUE(state(State::active));
    ASSERT_EQ(bluez.command("paired"), "ok"); ASSERT_EQ(bluez.command("NewConnection"), "allowed");
    HandoffFailure failure{}; auto peer = client->poll_bootstrap({1, 1}, failure); ASSERT_TRUE(peer);
    ASSERT_TRUE(client->get_ap_credentials({1, 1}, peer.value().handle));
    client->teardown(core::ConsumerId{1});
    EXPECT_FALSE(client->authorize_bootstrap({1, 1}, peer.value().handle, true, true));
    EXPECT_EQ(bluez.command("wire-read"), "closed");
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
    { const std::lock_guard lock{boundary->mutex};
      EXPECT_NE(boundary->rules.find("input tcp dport 5277 drop"), std::string::npos);
      EXPECT_NE(boundary->rules.find("input iifname \"wlan-test\" drop"), std::string::npos); }
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
    for (const char* channel : {"52", "149", "0", "44x", "44 extra"}) {
        save(directory / "ap.conf", std::string{"v1 wlan-test US jurisdiction-confirmed 5277 "} + channel + "\n");
        EXPECT_FALSE(load_policy(path, ::getuid())) << channel;
    }
    save(directory / "ap.conf", "v1 wlan-test US jurisdiction-confirmed 5277 44\n");
    const auto configured = load_policy(path, ::getuid()); ASSERT_TRUE(configured); EXPECT_EQ(configured.value().channel, 44U);
    save(directory / "ap.conf", "v1 wlan-test US jurisdiction-confirmed 5277\n");
    const auto policy = load_policy(path, ::getuid()); ASSERT_TRUE(policy); EXPECT_EQ(policy.value().channel, 36U);
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
