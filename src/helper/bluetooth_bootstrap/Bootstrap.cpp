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
#include <iostream>
#include <string>
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
const char* failure_name(HandoffFailure reason) {
    switch (reason) {
    case HandoffFailure::none: return "none";
    case HandoffFailure::denied: return "denied";
    case HandoffFailure::eligibility: return "eligibility";
    case HandoffFailure::eof: return "eof";
    case HandoffFailure::unknown_id: return "unknown_id";
    case HandoffFailure::wrong_order: return "wrong_order";
    case HandoffFailure::duplicate: return "duplicate";
    case HandoffFailure::oversize: return "oversize";
    case HandoffFailure::truncated: return "truncated";
    case HandoffFailure::protobuf: return "protobuf";
    case HandoffFailure::phone_rejected: return "phone_rejected";
    case HandoffFailure::timeout: return "timeout";
    case HandoffFailure::write: return "write";
    case HandoffFailure::snapshot: return "snapshot";
    case HandoffFailure::rebind: return "rebind";
    }
    return "unknown";
}
const char* message_name(std::uint16_t id) {
    switch (id) {
    case 1: return "WifiStartRequest";
    case 2: return "WifiInfoRequest";
    case 3: return "WifiInfoResponse";
    case 4: return "WifiVersionRequest";
    case 5: return "WifiVersionResponse";
    case 6: return "WifiConnectStatus";
    case 7: return "WifiStartResponse";
    case 8: return "WifiPingRequest";
    case 9: return "WifiPingResponse";
    case 10: return "WifiConnectionRejection";
    case 11: return "WifiSetupInfo";
    }
    return "unknown";
}
}
struct Bootstrap::SocketExpiry final {
    int socket;
    SnapshotDeadline deadline;
    std::mutex mutex;
    std::condition_variable changed;
    std::jthread worker;
    SocketExpiry(int fd, SnapshotDeadline until) : socket(::fcntl(fd, F_DUPFD_CLOEXEC, 0)), deadline(std::move(until)),
        worker([this](const std::stop_token& stop) {
            std::unique_lock lock{mutex};
            while (!stop.stop_requested()) {
                if (deadline.expired()) { if (socket >= 0) (void)::shutdown(socket, SHUT_RDWR); return; }
                changed.wait_for(lock, std::chrono::milliseconds(1));
            }
        }) {}
    ~SocketExpiry() { worker.request_stop(); changed.notify_all(); worker.join(); if (socket >= 0) ::close(socket); }
    // The join budget moves the one deadline later, once; the worker sees it on its next wake.
    void extend(Time until) { const std::lock_guard lock{mutex}; deadline.until = until; changed.notify_all(); }
};
Bootstrap::Bootstrap(int fd, Time accepted, std::function<Time()> now)
    : fd_(fd), deadline_{accepted + kBudget, std::move(now)}, expiry_(std::make_unique<SocketExpiry>(fd, deadline_)) {
    if (expiry_->socket < 0) fail(HandoffFailure::eligibility, std::nullopt, "could not duplicate the socket for its deadline");
}
// Destroying an unconfirmed bootstrap is an abort: the phone must see the channel close.
Bootstrap::~Bootstrap() { release(); wipe_bytes(output_); }
void Bootstrap::release() noexcept {
    expiry_.reset(); // joins the worker and closes its dup before fd_ can be closed
    if (fd_ < 0) return;
    (void)::shutdown(fd_, SHUT_RDWR); // reaches BlueZ's reference too: DISC (#161)
    ::close(fd_);
    fd_ = -1;
}
bool Bootstrap::expire() {
    if (live() && deadline_.expired()) fail(HandoffFailure::timeout, std::nullopt, "handshake deadline passed");
    // An unconfirmed completion frees the phone's channel at the backstop but keeps its
    // completed authority: RFCOMM teardown is not link loss (contract v2.6) and completed
    // authority has no TTL. The receiver retires the hand-off itself if it failed.
    else if (awaiting_confirmation() && deadline_.now() >= confirm_until_) release();
    return stage_ == Stage::failed;
}
void Bootstrap::deny(HandoffFailure reason) {
    if (stage_ == Stage::failed) return;
    failure_ = reason; stage_ = Stage::failed;
    release();
    wipe(snapshot_.password); wipe_bytes(output_);
}
const char* Bootstrap::phase() const {
    switch (stage_) {
    case Stage::authorization: return "authorization";
    case Stage::info: return "await_info_request";
    case Stage::start: return "await_start_response";
    case Stage::connect: return "await_connect_status";
    case Stage::finishing: return "finishing";
    case Stage::complete: return "complete";
    case Stage::failed: return "failed";
    }
    return "unknown";
}
// One line per failed handshake, then the ordinary deny path. Never prints the SSID, PSK, BSSID,
// endpoint or any peer address: only the phase, the record ID, the typed reason and a fixed detail.
void Bootstrap::fail(HandoffFailure reason, std::optional<std::uint16_t> id, std::string_view detail) {
    if (stage_ == Stage::failed) return;
    std::string line{"system-helper: wireless_handshake_failed phase="};
    line += phase(); line += " msg_id=";
    if (id) { line += std::to_string(*id); line += " msg="; line += message_name(*id); }
    else line += "none";
    line += " reason="; line += failure_name(reason);
    line += " detail=\""; line += detail; line += "\"\n";
    std::cerr << line << std::flush;
    deny(reason);
}
void Bootstrap::confirm() { if (complete()) confirmed_ = true; }
// One secret-free diagnostic line for a tolerated record (#170): never the payload.
void Bootstrap::note(const char* event, std::uint16_t id, std::string_view detail) const {
    std::string line{"system-helper: wireless_handshake_"};
    line += event; line += " phase="; line += phase();
    line += " msg_id="; line += std::to_string(id); line += " msg="; line += message_name(id);
    line += " detail=\""; line += detail; line += "\"\n";
    std::cerr << line << std::flush;
}
bool Bootstrap::queue(std::uint16_t id, const std::string& payload) {
    auto bytes = encode_aaw(id, {reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size()});
    if (!bytes) { fail(HandoffFailure::oversize, id, "outgoing record too large"); return false; }
    if (output_.empty()) sent_ = 0;
    output_.insert(output_.end(), bytes->begin(), bytes->end());
    wipe_bytes(*bytes); return true;
}
bool Bootstrap::authorize(const ApSnapshot& snapshot, bool ready, Time now) {
    if (expire()) return false;
    if (!ready) { fail(HandoffFailure::eligibility, std::nullopt, "projection listener not ready"); return false; }
    if (stage_ != Stage::authorization) return snapshot == snapshot_;
    if (fd_ < 0) return false;
    if (now >= deadline_.until || expire()) { fail(HandoffFailure::timeout, std::nullopt, "handshake deadline passed"); return false; }
    snapshot_ = snapshot;
    // Start-first (#166): the HU opens with WifiStartRequest{ip, port}, as aa-proxy-rs
    // (bluetooth.rs:6291-6298) and WirelessAndroidAutoDongle (bluetoothProfiles.cpp:40-44) do.
    aaw::WifiStartRequest start;
    start.set_ip_address(snapshot_.ip); start.set_port(snapshot_.port);
    stage_ = Stage::info; return queue(1, start.SerializeAsString());
}
bool Bootstrap::verify(const SnapshotProvider& provider) {
    if (!provider) { fail(HandoffFailure::snapshot, std::nullopt, "no AP snapshot provider"); return false; }
    if (expire()) return false;
    const auto current = provider();
    if (expire()) return false;
    if (!current || current.value() != snapshot_) { fail(HandoffFailure::snapshot, std::nullopt, "AP snapshot changed before release"); return false; }
    return true;
}
bool Bootstrap::flush(const SnapshotFence& fence) {
    if (output_.empty()) return true;
    ssize_t n{-1}; int error{};
    if (!fence || !fence(snapshot_.generation, [&] {
        if (expire()) return;
        n = ::send(fd_, output_.data() + sent_, output_.size() - sent_, MSG_DONTWAIT | MSG_NOSIGNAL);
        error = errno;
    })) { fail(HandoffFailure::snapshot, std::nullopt, "AP generation fence failed before send"); return false; }
    if (fd_ < 0) return false;
    if (n < 0) {
        if (error == EAGAIN || error == EWOULDBLOCK || error == EINTR) return false;
        fail(HandoffFailure::write, std::nullopt, "socket send failed"); return false;
    }
    if (n == 0) { fail(HandoffFailure::write, std::nullopt, "socket send wrote nothing"); return false; }
    sent_ += static_cast<std::size_t>(n);
    if (sent_ != output_.size()) return false;
    wipe_bytes(output_);
    // Credentials are out: give the phone its Wi-Fi join, once, if the first budget still holds.
    if (stage_ == Stage::start && !joining_) {
        joining_ = true;
        if (deadline_.expired()) { fail(HandoffFailure::timeout, std::nullopt, "handshake deadline passed"); return false; }
        deadline_.until = deadline_.now() + kJoinBudget;
        if (expiry_) expiry_->extend(deadline_.until);
    }
    return true;
}
// The only record that carries the AP secret; a re-send is byte-identical (same pinned snapshot).
bool Bootstrap::queue_info() {
    aaw::WifiInfoResponse response;
    response.set_ssid(snapshot_.ssid); response.set_password(snapshot_.password); response.set_bssid(snapshot_.bssid);
    response.set_security_mode(aaw::WPA2_PERSONAL); response.set_access_point_type(aaw::DYNAMIC);
    auto payload = response.SerializeAsString();
    const auto queued = queue(3, payload); wipe(payload); wipe(*response.mutable_password());
    return queued;
}
void Bootstrap::record(const AawRecord& record) {
    const auto id = record.id;
    // Tolerated like the references (#170 review): the dongles read the phone's records without
    // validating them (WirelessAndroidAutoDongle bluetoothProfiles.cpp:63-64; aa-proxy-rs only
    // warns on an unexpected ID, bluetooth.rs:4724-4729, and its MITM path absorbs pings,
    // :1044-1067). Pings (8/9), SetupInfo (11) and unknown IDs are ignored, bounded.
    if (id == 8 || id == 9 || id == 11 || id == 0 || id > 11) {
        if (++ignored_ > kIgnoredLimit) { fail(HandoffFailure::unknown_id, id, "too many ignored records"); return; }
        note("ignored", id, "not handled by the head unit");
        return;
    }
    if (id == 10) { fail(HandoffFailure::phone_rejected, id, "phone sent WifiConnectionRejection"); return; }
    if (id == 2 && seen_info_ && (stage_ == Stage::start || stage_ == Stage::connect)) {
        // A repeated WifiInfoRequest gets the identical response again, bounded (aa-proxy-rs
        // re-sends its cached WifiInfoResponse, bluetooth.rs:1069-1086).
        aaw::WifiInfoRequest request;
        if (!parse(request, record)) { fail(HandoffFailure::protobuf, id, "payload did not parse"); return; }
        if (++info_resends_ > kInfoResendLimit) { fail(HandoffFailure::duplicate, id, "WifiInfoRequest repeated too often"); return; }
        if (queue_info()) note("resent", 3, "repeated WifiInfoRequest");
        return;
    }
    if ((id == 2 && seen_info_) || (id == 7 && seen_start_)) { fail(HandoffFailure::duplicate, id, "message repeated"); return; }
    if (stage_ == Stage::info && id == 2) {
        // aa-proxy-rs bluetooth.rs:6301-6314; WirelessAndroidAutoDongle bluetoothProfiles.cpp:46-61.
        aaw::WifiInfoRequest request;
        if (!parse(request, record)) { fail(HandoffFailure::protobuf, id, "payload did not parse"); return; }
        if (!queue_info()) return;
        seen_info_ = true; stage_ = Stage::start;
    } else if (stage_ == Stage::start && id == 7) {
        // aa-proxy-rs bluetooth.rs:6317 reads it without checking the status (tag 3, :919-923), and
        // OAA maps 1 to success (WifiVersionStatusEnum.proto:18-31). Only a negative value, which
        // every source treats as a failure, is one here; an absent status is accepted.
        aaw::WifiStartResponse response;
        if (!parse(response, record)) { fail(HandoffFailure::protobuf, id, "payload did not parse (status is field 3)"); return; }
        if (response.has_status() && response.status() < 0) {
            fail(HandoffFailure::phone_rejected, id, "phone status " + std::to_string(response.status())); return;
        }
        if (!response.has_status()) note("accepted", id, "no status");
        else if (response.status() != aaw::STATUS_SUCCESS) note("accepted", id, "status " + std::to_string(response.status()));
        seen_start_ = true; stage_ = Stage::connect;
    } else if (stage_ == Stage::connect && id == 6) {
        // aa-proxy-rs bluetooth.rs:6320, success [08 00] at 4746-4754, status tag 1 at 914-917.
        aaw::WifiConnectStatus status;
        if (!parse(status, record)) { fail(HandoffFailure::protobuf, id, "payload did not parse (status is field 1)"); return; }
        if (status.status() != aaw::STATUS_SUCCESS) {
            fail(HandoffFailure::phone_rejected, id, "phone status " + std::to_string(status.status())); return;
        }
        stage_ = Stage::finishing;
    } else if (id == 4 || id == 5) {
        fail(HandoffFailure::wrong_order, id, "version negotiation is not used; the HU runs the start-first flow");
    } else {
        fail(HandoffFailure::wrong_order, id, "unexpected message for this phase");
    }
}
void Bootstrap::tick(Time now, const SnapshotProvider& provider, const SnapshotFence& fence, bool single_step) {
    if (fd_ < 0 || stage_ == Stage::complete) return;
    if (now >= deadline_.until || expire()) { fail(HandoffFailure::timeout, std::nullopt, "handshake deadline passed"); return; }
    if (stage_ == Stage::authorization) {
        // Early phone bytes stay queued and are judged by the same rules after the start request.
        std::uint8_t byte{};
        const auto n = ::recv(fd_, &byte, 1, MSG_DONTWAIT | MSG_PEEK);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            fail(HandoffFailure::eof, std::nullopt, "socket closed before authorization");
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
            fail(HandoffFailure::eof, std::nullopt, "socket receive failed"); return;
        }
        if (n == 0) {
            const auto pending = decoder_.pending_id();
            const auto error = decoder_.finish();
            if (error) fail(decode_error(*error), pending, "phone closed the socket inside a record");
            else fail(HandoffFailure::eof, std::nullopt, "phone closed the socket");
            return;
        }
        decoder_.feed({bytes.data(), static_cast<std::size_t>(n)});
        if (!drain(provider, fence, single_step) || single_step) return;
    }
}
bool Bootstrap::drain(const SnapshotProvider& provider, const SnapshotFence& fence, bool single_step) {
    for (;;) {
        const auto next = decoder_.next();
        if (std::holds_alternative<std::monostate>(next)) return true;
        if (const auto* error = std::get_if<AawError>(&next)) { fail(decode_error(*error), decoder_.pending_id(), "record header rejected"); return false; }
        record(std::get<AawRecord>(next));
        if (fd_ < 0) return false;
        if (single_step && !output_.empty()) return false;
        if (!output_.empty() && (!verify(provider) || !flush(fence))) return false;
        if (stage_ == Stage::finishing) {
            // A successful WifiConnectStatus ends the exchange. Anything the phone sends after it is
            // never read (#170 review: the dongles stop reading there too), but the deadline holds.
            expiry_.reset();
            if (expire()) return false;
            // Keep the socket until the hand-off is confirmed or aborted (see Bootstrap.hpp).
            confirm_until_ = deadline_.now() + kConfirmBudget;
            stage_ = Stage::complete;
            std::cerr << "system-helper: wireless_handshake_complete flow=start_first\n" << std::flush;
            return false;
        }
        if (single_step) return false;
    }
}
}
