// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <span>

namespace aa::ipc {
inline constexpr std::size_t payload_limit = 16UL * 1024;
inline constexpr std::size_t frame_limit = 4UL * 1024 * 1024;
inline constexpr std::size_t header_size = 56;
inline constexpr int socket_budget = 256 * 1024;
using Packet = std::array<std::uint8_t, header_size + payload_limit>;
struct Header {
    std::uint64_t frame{}, sequence{}, timestamp_ns{};
    std::uint32_t index{}, count{}, frame_bytes{}, payload_bytes{};
    bool idr{};
};
void encode(Packet& packet, const Header& header);
bool decode(std::span<const std::uint8_t> packet, Header& header);
std::uint64_t monotonic_ns();
enum class Receive { rejected, waiting, fragment, complete };
class Reassembler {
public:
    Receive accept(std::span<const std::uint8_t> packet);
    void loss();
    std::span<const std::uint8_t> frame() const { return {bytes_.data(), used_}; }
    const Header& header() const { return current_; }
    std::uint64_t rejected{}, resyncs{}, suppressed{};
    bool waiting_idr() const { return waiting_; }
private:
    std::array<std::uint8_t, frame_limit> bytes_{};
    Header current_{};
    std::size_t used_{};
    std::uint32_t next_{};
    std::uint64_t sequence_{}, last_frame_{};
    bool have_sequence_{}, have_frame_{}, active_{}, waiting_{true};
};
enum class Send { sent, dropped, invalid, error };
class Sender {
public:
    explicit Sender(int fd) : fd_(fd) {}
    Send send_frame(std::span<const std::uint8_t> frame, std::uint64_t id,
                    bool idr, std::uint64_t timestamp);
    std::uint64_t drops{}, backpressure{};
private:
    int fd_;
    std::uint64_t sequence_{};
    bool waiting_{true};
    Packet packet_{};
};
class SocketPair {
public:
    SocketPair();
    ~SocketPair();
    SocketPair(const SocketPair&) = delete;
    SocketPair& operator=(const SocketPair&) = delete;
    int producer() const { return fds_[0]; }
    int consumer() const { return fds_[1]; }
    int send_buffer() const { return send_buffer_; }
private:
    int fds_[2]{-1, -1};
    int send_buffer_{};
};
// recvmsg reports MSG_TRUNC; oversized packets are rejected, never resized.
Receive receive(int fd, Packet& packet, Reassembler& assembler, bool& eof);
}
