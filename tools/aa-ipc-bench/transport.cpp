// SPDX-License-Identifier: GPL-3.0-or-later
#include "transport.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace aa::ipc {
namespace {
void put(Packet& p, std::size_t offset, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) p[offset + i] = static_cast<std::uint8_t>(value >> (8 * (size - i - 1)));
}
template<unsigned size>
std::uint64_t get(std::span<const std::uint8_t> p, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | p[offset + i];
    return value;
}
}
std::uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void encode(Packet& p, const Header& h) {
    put(p, 0, 0x41415631, 4); put(p, 4, 1, 2); put(p, 6, header_size, 2);
    put(p, 8, h.idr ? 1 : 0, 4); put(p, 12, h.frame, 8);
    put(p, 20, h.index, 4); put(p, 24, h.count, 4);
    put(p, 28, h.sequence, 8); put(p, 36, h.timestamp_ns, 8);
    put(p, 44, h.frame_bytes, 4); put(p, 48, h.payload_bytes, 4); put(p, 52, 0, 4);
}
bool decode(std::span<const std::uint8_t> p, Header& h) {
    if (p.size() < header_size || p.size() > header_size + payload_limit ||
        get<4>(p, 0) != 0x41415631 || get<2>(p, 4) != 1 ||
        get<2>(p, 6) != header_size || get<4>(p, 8) > 1 || get<4>(p, 52) != 0) return false;
    h = {get<8>(p, 12), get<8>(p, 28), get<8>(p, 36),
         static_cast<std::uint32_t>(get<4>(p, 20)), static_cast<std::uint32_t>(get<4>(p, 24)),
         static_cast<std::uint32_t>(get<4>(p, 44)), static_cast<std::uint32_t>(get<4>(p, 48)), get<4>(p, 8) == 1};
    if (!h.frame_bytes || h.frame_bytes > frame_limit || !h.payload_bytes ||
        h.payload_bytes > payload_limit || p.size() != header_size + h.payload_bytes ||
        h.count != (h.frame_bytes + payload_limit - 1) / payload_limit || h.index >= h.count) return false;
    const auto expected = std::min(payload_limit, h.frame_bytes - h.index * payload_limit);
    return h.payload_bytes == expected;
}
void Reassembler::loss() { waiting_ = true; active_ = false; used_ = 0; next_ = 0; }
Receive Reassembler::accept(std::span<const std::uint8_t> p) {
    Header h;
    if (!decode(p, h)) { ++rejected; loss(); return Receive::rejected; }
    if (have_sequence_ && (sequence_ == UINT64_MAX || h.sequence != sequence_ + 1)) loss();
    sequence_ = h.sequence; have_sequence_ = true;
    if (h.index == 0) {
        if (active_) loss();
        if (have_frame_ && h.frame <= last_frame_) { ++rejected; loss(); return Receive::rejected; }
        last_frame_ = h.frame; have_frame_ = true;
        if (waiting_ && !h.idr) { ++suppressed; return Receive::waiting; }
        current_ = h; used_ = 0; next_ = 0; active_ = true;
    }
    if (!active_) { ++suppressed; return Receive::waiting; }
    if (h.frame != current_.frame || h.index != next_ || h.count != current_.count ||
        h.frame_bytes != current_.frame_bytes || h.idr != current_.idr || h.timestamp_ns != current_.timestamp_ns) {
        ++rejected; loss(); return Receive::rejected;
    }
    std::memcpy(bytes_.data() + used_, p.data() + header_size, h.payload_bytes);
    used_ += h.payload_bytes; ++next_;
    if (next_ != h.count) return Receive::fragment;
    active_ = false;
    if (waiting_) { ++resyncs; waiting_ = false; }
    return Receive::complete;
}
Send Sender::send_frame(std::span<const std::uint8_t> bytes, std::uint64_t id, bool idr, std::uint64_t timestamp) {
    if (bytes.empty() || bytes.size() > frame_limit) return Send::invalid;
    if (waiting_ && !idr) { ++drops; return Send::dropped; }
    const auto count = static_cast<std::uint32_t>((bytes.size() + payload_limit - 1) / payload_limit);
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto length = std::min(payload_limit, bytes.size() - index * payload_limit);
        // Consume sequence numbers even for unsent fragments so loss is visible at the peer.
        Header h{id, sequence_++, timestamp, index, count, static_cast<std::uint32_t>(bytes.size()),
                 static_cast<std::uint32_t>(length), idr};
        encode(packet_, h);
        std::memcpy(packet_.data() + header_size, bytes.data() + index * payload_limit, length);
        ssize_t result;
        do { result = ::send(fd_, packet_.data(), header_size + length, MSG_DONTWAIT | MSG_NOSIGNAL); } while (result < 0 && errno == EINTR);
        if (result != static_cast<ssize_t>(header_size + length)) {
            waiting_ = true; ++drops;
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) { ++backpressure; return Send::dropped; }
            return Send::error;
        }
    }
    waiting_ = false; return Send::sent;
}
SocketPair::SocketPair() {
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds_) != 0) throw std::runtime_error("socketpair failed");
    for (int fd : fds_) {
        if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_budget, sizeof(socket_budget)) ||
            ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_budget, sizeof(socket_budget))) {
            ::close(fds_[0]); ::close(fds_[1]); throw std::runtime_error("socket bounds failed");
        }
    }
    socklen_t length = sizeof(send_buffer_);
    if (::getsockopt(fds_[0], SOL_SOCKET, SO_SNDBUF, &send_buffer_, &length) || send_buffer_ > socket_budget * 2) {
        ::close(fds_[0]); ::close(fds_[1]); throw std::runtime_error("socket exceeds bound");
    }
}
SocketPair::~SocketPair() { ::close(fds_[0]); ::close(fds_[1]); }
Receive receive(int fd, Packet& packet, Reassembler& assembler, bool& eof) {
    iovec vector{packet.data(), packet.size()}; msghdr message{};
    message.msg_iov = &vector; message.msg_iovlen = 1;
    ssize_t size;
    do { size = ::recvmsg(fd, &message, 0); } while (size < 0 && errno == EINTR);
    eof = size == 0;
    if (size < 0) throw std::runtime_error("recvmsg failed");
    if (eof) return Receive::waiting;
    if (message.msg_flags & MSG_TRUNC) { ++assembler.rejected; assembler.loss(); return Receive::rejected; }
    return assembler.accept({packet.data(), static_cast<std::size_t>(size)});
}
}
