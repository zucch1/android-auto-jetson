// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/Bootstrap.hpp>
#include "Bootstrap.pb.h"
#include <array>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <condition_variable>
#include <thread>

namespace aa::helper {
void wipe(std::string& value) noexcept { if (!value.empty()) ::explicit_bzero(value.data(), value.size()); value.clear(); }
namespace {
void wipe_bytes(std::vector<std::uint8_t>& value) { if (!value.empty()) ::explicit_bzero(value.data(), value.size()); value.clear(); }
HandoffFailure decode_error(AawError error) {
    switch (error) {
    case AawError::oversize: return HandoffFailure::oversize;
    case AawError::unknown_id: return HandoffFailure::unknown_id;
    case AawError::truncated: return HandoffFailure::truncated;
    }
    return HandoffFailure::truncated;
}
template<class T> bool parse(T& message, const AawRecord& record) {
    return message.ParsePartialFromArray(record.payload.data(), static_cast<int>(record.payload.size())) && message.IsInitialized();
}
}
struct Bootstrap::SocketExpiry final {
    int socket;
    std::mutex mutex;
    std::condition_variable changed;
    std::jthread worker;
    SocketExpiry(int fd, SnapshotDeadline deadline) : socket(::fcntl(fd, F_DUPFD_CLOEXEC, 0)),
        worker([this, deadline = std::move(deadline)](const std::stop_token& stop) {
            std::unique_lock lock{mutex};
            while (!stop.stop_requested()) {
                if (deadline.expired()) { if (socket >= 0) (void)::shutdown(socket, SHUT_RDWR); return; }
                changed.wait_for(lock, std::chrono::milliseconds(1));
            }
        }) {}
    ~SocketExpiry() { worker.request_stop(); changed.notify_all(); worker.join(); if (socket >= 0) ::close(socket); }
};
Bootstrap::Bootstrap(int fd, Time accepted, std::function<Time()> now)
    : fd_(fd), deadline_{accepted + kBudget, std::move(now)}, expiry_(std::make_unique<SocketExpiry>(fd, deadline_)) {
    if (expiry_->socket < 0) deny(HandoffFailure::eligibility);
}
Bootstrap::~Bootstrap() { expiry_.reset(); if (fd_ >= 0) ::close(fd_); wipe_bytes(output_); }
bool Bootstrap::expire() {
    if (fd_ >= 0 && deadline_.expired()) deny(HandoffFailure::timeout);
    return stage_ == Stage::failed;
}
void Bootstrap::deny(HandoffFailure reason) {
    if (stage_ == Stage::failed) return;
    failure_ = reason; stage_ = Stage::failed;
    expiry_.reset();
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1; wipe(snapshot_.password); wipe_bytes(output_);
}
bool Bootstrap::queue(std::uint16_t id, const std::string& payload) {
    // The shared legacy codec only knows IDs 1..7; SetupInfo uses the same framing.
    auto bytes = encode_aaw(id == 11 ? 1 : id, {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
    if (!bytes) { deny(HandoffFailure::oversize); return false; }
    if (id == 11) (*bytes)[3] = 11;
    if (output_.empty()) sent_ = 0;
    output_.insert(output_.end(), bytes->begin(), bytes->end());
    wipe_bytes(*bytes); return true;
}
bool Bootstrap::authorize(const ApSnapshot& snapshot, bool ready, Time now) {
    if (expire()) return false;
    if (!ready) { deny(HandoffFailure::eligibility); return false; }
    if (stage_ != Stage::authorization) return snapshot == snapshot_;
    if (fd_ < 0) return false;
    if (now >= deadline_.until || expire()) { deny(HandoffFailure::timeout); return false; }
    snapshot_ = snapshot;
    aaw::WifiVersionRequest message;
    message.set_major_version(1); message.set_minor_version(0);
    message.set_selected_channel(aaw::CHANNEL_5GHZ_ONLY);
    stage_ = Stage::info; return queue(4, message.SerializeAsString());
}
bool Bootstrap::verify(const SnapshotProvider& provider) {
    if (!provider) { deny(HandoffFailure::snapshot); return false; }
    if (expire()) return false;
    const auto current = provider();
    if (expire()) return false;
    if (!current || current.value() != snapshot_) { deny(HandoffFailure::snapshot); return false; }
    return true;
}
bool Bootstrap::flush(const SnapshotFence& fence) {
    if (output_.empty()) return true;
    ssize_t n{-1}; int error{};
    if (!fence || !fence(snapshot_.generation, [&] {
        if (expire()) return;
        n = ::send(fd_, output_.data() + sent_, output_.size() - sent_, MSG_DONTWAIT | MSG_NOSIGNAL);
        error = errno;
    })) { deny(HandoffFailure::snapshot); return false; }
    if (fd_ < 0) return false;
    if (n < 0) {
        if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) return false;
        deny(HandoffFailure::write); return false;
    }
    if (n == 0) { deny(HandoffFailure::write); return false; }
    sent_ += static_cast<std::size_t>(n);
    if (sent_ != output_.size()) return false;
    wipe_bytes(output_); return true;
}
void Bootstrap::record(const AawRecord& record) {
    if (((record.id == 2 || record.id == 5) && seen_info_) || (record.id == 7 && seen_start_)) { deny(HandoffFailure::duplicate); return; }
    if (stage_ == Stage::info && record.id == 5) {
        aaw::WifiVersionResponse response;
        if (!parse(response, record)) { deny(HandoffFailure::protobuf); return; }
        if (response.status() != aaw::SUCCESS) { deny(HandoffFailure::phone_rejected); return; }
        if (response.major_version() != 1 || response.minor_version() != 0) { deny(HandoffFailure::phone_rejected); return; }
        aaw::WifiSetupInfo setup;
        setup.set_major_version(1); setup.set_minor_version(0);
        setup.mutable_endpoint()->set_ip_address(snapshot_.ip); setup.mutable_endpoint()->set_port(snapshot_.port);
        auto* ap = setup.mutable_access_point();
        ap->set_ssid(snapshot_.ssid); ap->set_bssid(snapshot_.bssid); ap->set_password(snapshot_.password);
        ap->set_security_mode(aaw::WPA2_PERSONAL);
        auto payload = setup.SerializeAsString();
        const auto queued = queue(11, payload); wipe(payload); wipe(*ap->mutable_password());
        if (!queued) return;
        aaw::ModernWifiStartRequest start;
        start.set_ip_address(snapshot_.ip); start.set_port(snapshot_.port); start.set_reason(aaw::DYNAMIC);
        if (!queue(1, start.SerializeAsString())) return;
        modern_ = true; seen_info_ = true; stage_ = Stage::start;
    } else if (stage_ == Stage::info && record.id == 2) {
        aaw::WifiInfoRequest request;
        if (!parse(request, record)) { deny(HandoffFailure::protobuf); return; }
        aaw::WifiInfoResponse response;
        response.set_ssid(snapshot_.ssid); response.set_password(snapshot_.password); response.set_bssid(snapshot_.bssid);
        response.set_security_mode(aaw::WPA2_PERSONAL); response.set_access_point_type(aaw::DYNAMIC);
        auto payload = response.SerializeAsString();
        const auto queued = queue(3, payload); wipe(payload); wipe(*response.mutable_password());
        if (!queued) return;
        aaw::WifiStartRequest start; start.set_ip_address(snapshot_.ip); start.set_port(snapshot_.port);
        if (!queue(1, start.SerializeAsString())) return;
        seen_info_ = true; stage_ = Stage::start;
    } else if (stage_ == Stage::start && record.id == 7) {
        aaw::WifiStartResponse legacy;
        aaw::ModernWifiStartResponse modern;
        if (modern_ ? !parse(modern, record) : !parse(legacy, record)) { deny(HandoffFailure::protobuf); return; }
        if (modern_ ? modern.status() != aaw::SUCCESS : legacy.status() != aaw::STATUS_SUCCESS) { deny(HandoffFailure::phone_rejected); return; }
        seen_start_ = true; stage_ = Stage::connect;
    } else if (stage_ == Stage::connect && record.id == 6) {
        aaw::WifiConnectStatus legacy;
        aaw::ModernWifiConnectStatus modern;
        if (modern_ ? !parse(modern, record) : !parse(legacy, record)) { deny(HandoffFailure::protobuf); return; }
        if (modern_ ? modern.status() != aaw::SUCCESS : legacy.status() != aaw::STATUS_SUCCESS) { deny(HandoffFailure::phone_rejected); return; }
        stage_ = Stage::finishing;
    } else { deny(HandoffFailure::wrong_order); }
}
void Bootstrap::tick(Time now, const SnapshotProvider& provider, const SnapshotFence& fence, bool single_step) {
    if (fd_ < 0 || stage_ == Stage::complete) return;
    if (now >= deadline_.until || expire()) { deny(HandoffFailure::timeout); return; }
    if (stage_ == Stage::authorization) {
        std::uint8_t byte{};
        const auto n = ::recv(fd_, &byte, 1, MSG_DONTWAIT | MSG_PEEK);
        if (n > 0) deny(HandoffFailure::wrong_order);
        else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) deny(HandoffFailure::eof);
        return;
    }
    if (!output_.empty()) {
        if (!verify(provider) || !flush(fence) || single_step) return;
    }
    if (!drain(provider, fence, single_step)) return;
    std::array<std::uint8_t, 4100> bytes{};
    for (unsigned iteration = 0; iteration < 4 && fd_ >= 0; ++iteration) {
        const auto n = ::recv(fd_, bytes.data(), bytes.size(), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
            deny(HandoffFailure::eof); return;
        }
        if (n == 0) {
            const auto error = decoder_.finish(); deny(error ? decode_error(*error) : HandoffFailure::eof); return;
        }
        decoder_.feed({bytes.data(), static_cast<std::size_t>(n)});
        if (!drain(provider, fence, single_step) || single_step) return;
    }
}
bool Bootstrap::drain(const SnapshotProvider& provider, const SnapshotFence& fence, bool single_step) {
    for (;;) {
        const auto next = decoder_.next();
        if (std::holds_alternative<std::monostate>(next)) return true;
        if (const auto* error = std::get_if<AawError>(&next)) { deny(decode_error(*error)); return false; }
        record(std::get<AawRecord>(next));
        if (fd_ < 0) return false;
        if (single_step && !output_.empty()) return false;
        if (!output_.empty() && (!verify(provider) || !flush(fence))) return false;
        if (stage_ == Stage::finishing) {
            if (!std::holds_alternative<std::monostate>(decoder_.next())) { deny(HandoffFailure::duplicate); return false; }
            if (const auto error = decoder_.finish()) { deny(decode_error(*error)); return false; }
            expiry_.reset();
            std::uint8_t byte{};
            const auto n = ::recv(fd_, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
            const auto error = errno;
            if (expire()) return false;
            if (n > 0) { deny(HandoffFailure::truncated); return false; }
            if (n < 0 && error != EAGAIN && error != EWOULDBLOCK) { deny(HandoffFailure::eof); return false; }
            ::close(fd_); fd_ = -1;
            stage_ = Stage::complete; return false;
        }
        if (single_step) return false;
    }
}
}
