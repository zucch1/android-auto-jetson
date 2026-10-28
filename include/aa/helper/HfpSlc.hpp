// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace aa::helper {
// Minimal AG service-level negotiation only: no call control or audio support.
class HfpSlc {
public:
    enum class State { brsf, cind_test, cind_read, cmer, connected, closed };
    static constexpr std::size_t max_command_size = 256;
    static constexpr std::size_t tick_budget = 512;

    // Takes sole ownership of a connected RFCOMM stream socket, including on failure.
    // The caller must not read/write/close it afterward. No fd flags are changed.
    explicit HfpSlc(int owned_fd) noexcept;
    ~HfpSlc();
    HfpSlc(const HfpSlc&) = delete;
    HfpSlc& operator=(const HfpSlc&) = delete;
    HfpSlc(HfpSlc&&) = delete;
    HfpSlc& operator=(HfpSlc&&) = delete;

    // At most tick_budget nonblocking socket operations, including interrupted ones.
    // Call regularly while open, even during negotiation. EOF/errors close the fd.
    void tick() noexcept;
    [[nodiscard]] State state() const noexcept { return state_; }
    [[nodiscard]] bool connected() const noexcept { return state_ == State::connected; }

private:
    void close() noexcept;
    void command() noexcept;
    int fd_;
    State state_{State::brsf};
    State after_reply_{State::brsf};
    std::array<char, max_command_size> input_{};
    std::size_t input_size_{};
    // Replies refer only to static literals, so queued output is bounded and allocation-free.
    std::string_view output_;
    bool after_cr_{};
};
}
