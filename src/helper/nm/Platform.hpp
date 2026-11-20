// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <aa/helper/SystemHelper.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <gio/gio.h>

namespace aa::helper::nm {
struct Policy final {
    std::string interface;
    std::string country;
    std::uint16_t port{};
    std::string state_directory;
    std::uint32_t owner{};
    std::uint32_t channel{36};
};
// #89 L9: the WPA2 PSK (64 lowercase hex digits + NUL) lives in one fixed
// in-object buffer, written in place and cleansed on destruction. It is never
// built in, or copied through, a growable std::string. Copies are bounded
// value copies that each cleanse themselves.
class Psk final {
public:
    static constexpr std::size_t kLength = 64;
    Psk() noexcept = default;
    Psk(const Psk& other) noexcept : bytes_(other.bytes_) {}
    Psk& operator=(const Psk& other) noexcept { bytes_ = other.bytes_; return *this; }
    ~Psk() { ::explicit_bzero(bytes_.data(), bytes_.size()); }
    // The digits to fill in place; the terminating NUL is never writable.
    [[nodiscard]] std::span<char, kLength> digits() noexcept { return std::span<char, kLength>{bytes_.data(), kLength}; }
    [[nodiscard]] std::string_view view() const noexcept { return {bytes_.data(), kLength}; }
    [[nodiscard]] const char* c_str() const noexcept { return bytes_.data(); }
    friend bool operator==(const Psk& a, const Psk& b) noexcept { return a.bytes_ == b.bytes_; }
private:
    std::array<char, kLength + 1> bytes_{};
};
struct Credentials final { std::string ssid; Psk psk; };
core::Result<Policy> load_policy(const std::string& directory, std::uint32_t owner);
core::Result<Credentials> credentials(const Policy& policy, bool rotate);
// Per-activation credentials, never written to disk.
core::Result<Credentials> fresh_credentials();
// The PSK as a floating D-Bus string GVariant backed by one private 65-byte
// buffer that GLib reads in place (never copies) and that is cleansed when the
// last reference is released, instead of g_variant_new_string's unwiped
// g_strdup (#89 L9).
GVariant* psk_variant(const Psk& psk);
class Commands {
public:
    virtual ~Commands() = default;
    virtual core::Result<std::string> run(const std::vector<std::string>& args,
                                          const std::string& input = {}) = 0;
};
std::unique_ptr<Commands> production_commands();
// Non-root private-bus mode only: framed AF_UNIX command boundary, never exec.
std::unique_ptr<Commands> private_commands(const std::string& socket);
bool eligible(const Policy& policy, Commands& commands);
bool actual_channel(const Policy& policy, Commands& commands);
std::string firewall(const Policy& policy);
class Connection final {
public:
    explicit Connection(const std::string& address);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    bool healthy() const;
    GVariant* call(const std::string& path, const char* interface, const char* method,
                   GVariant* args, const GVariantType* result, GError** error = nullptr);
    std::string device(const std::string& interface);
    std::string text_property(const std::string& path, const char* interface, const char* property);
    unsigned number_property(const std::string& path, const char* interface, const char* property);
    bool start(const Policy& policy, const Credentials& credentials, const std::string& device);
    bool stop(const std::string& device);
    bool active() const;
    bool identity_current(const Policy&, const std::string& device);
    core::Result<std::pair<std::string, std::string>> endpoint(const Policy&, const std::string& device);
    void collection_deadline(SnapshotDeadline deadline) { collection_deadline_ = std::move(deadline); }
private:
    GDBusConnection* bus_{};
    std::string owner_, active_, profile_;
    bool unknown_activation_{};
    bool deactivation_ack_{};
    SnapshotDeadline collection_deadline_;
    int call_timeout() const;
};
class Backend final : public helper::Backend {
public:
    Backend(Policy policy, std::string address, std::unique_ptr<Commands> commands);
    ~Backend() override;
    core::Result<State> apply(Action action, std::uint32_t channel) override;
    void cleanup() noexcept override;
    core::Result<void> revoke(std::uint64_t phone) override;
    core::Result<ApSnapshot> ap_snapshot() override;
    core::Result<ApSnapshot> ap_snapshot(const SnapshotDeadline&) override;
    bool snapshot_current(std::uint64_t) override;
    bool with_snapshot(std::uint64_t, const std::function<void()>&) override;
    void rekey(std::uint64_t) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
