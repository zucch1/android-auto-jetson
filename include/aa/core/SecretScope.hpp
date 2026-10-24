// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstring>

namespace aa::core {
template<class Buffer> class SecretScope final {
public:
    explicit SecretScope(Buffer& buffer, bool enabled = true) : buffer_(buffer), enabled_(enabled) {}
    SecretScope(const SecretScope&) = delete;
    SecretScope& operator=(const SecretScope&) = delete;
    ~SecretScope() {
        if (enabled_ && !buffer_.empty()) ::explicit_bzero(buffer_.data(), buffer_.size());
    }
    void release() noexcept { enabled_ = false; }
private:
    Buffer& buffer_;
    bool enabled_;
};
}
