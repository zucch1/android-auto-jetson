// SPDX-License-Identifier: GPL-3.0-or-later
#include <aa/helper/HfpSlc.hpp>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <sys/socket.h>
#include <unistd.h>

namespace aa::helper {
namespace {
constexpr std::string_view kOk = "\r\nOK\r\n";
constexpr std::string_view kError = "\r\nERROR\r\n";
constexpr std::string_view kBrsf = "\r\n+BRSF:0\r\n\r\nOK\r\n";
constexpr std::string_view kIndicators =
    "\r\n+CIND:(\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),"
    "(\"callheld\",(0-2)),(\"signal\",(0-5)),(\"roam\",(0,1)),"
    "(\"battchg\",(0-5))\r\n\r\nOK\r\n";
// No telephony service, active calls, signal, roaming, or battery telemetry.
constexpr std::string_view kValues = "\r\n+CIND:0,0,0,0,0,0,0\r\n\r\nOK\r\n";
}

HfpSlc::HfpSlc(int owned_fd) noexcept : fd_(owned_fd) {
    if (fd_ < 0) state_ = State::closed;
}

HfpSlc::~HfpSlc() { close(); }

void HfpSlc::close() noexcept {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
    state_ = State::closed;
    output_ = {};
    input_size_ = 0;
}

void HfpSlc::command() noexcept {
    const std::string_view line{input_.data(), input_size_};
    input_size_ = 0;
    after_reply_ = state_;
    switch (state_) {
    case State::brsf: {
        constexpr std::string_view prefix = "AT+BRSF=";
        if (!line.starts_with(prefix)) break;
        const auto digits = line.substr(prefix.size());
        if (digits.empty()) break;
        std::uint32_t features{};
        const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), features);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size()) break;
        output_ = kBrsf;
        after_reply_ = State::cind_test;
        return;
    }
    case State::cind_test:
        if (line != "AT+CIND=?") break;
        output_ = kIndicators;
        after_reply_ = State::cind_read;
        return;
    case State::cind_read:
        if (line != "AT+CIND?") break;
        output_ = kValues;
        after_reply_ = State::cmer;
        return;
    case State::cmer:
        if (line != "AT+CMER=3,0,0,1") break;
        output_ = kOk;
        after_reply_ = State::connected;
        return;
    case State::connected:
        if (!line.starts_with("AT") || line.size() < 3) break;
        output_ = kError;
        return;
    case State::closed:
        return;
    }
    close();
}

void HfpSlc::tick() noexcept {
    for (std::size_t operations = 0; operations < tick_budget && fd_ >= 0; ++operations) {
        if (!output_.empty()) {
            const auto count = ::send(fd_, output_.data(), output_.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            if (count > 0) {
                output_.remove_prefix(static_cast<std::size_t>(count));
                if (output_.empty()) state_ = after_reply_;
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            close();
            return;
        }
        char byte{};
        const auto count = ::recv(fd_, &byte, 1, MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (count <= 0) {
            close();
            return;
        }
        if (after_cr_ && byte == '\n') {
            after_cr_ = false;
            continue;
        }
        after_cr_ = byte == '\r';
        if (after_cr_) {
            command();
        } else if (byte < ' ' || byte > '~' || input_size_ == input_.size()) {
            close();
        } else {
            input_[input_size_++] = byte;
        }
    }
}
}
