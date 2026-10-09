// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal project-owned C++20 cross-build smoke: proves the toolchain compiles and links an
// aarch64 binary inside the validated sysroot. It is a build-surface probe only and carries
// no Android Auto behavior; cross-build acceptance runs against the observed sysroot, not here.
#include <cstdio>
#include <optional>
#include <span>
#include <array>

namespace {
int sum(const std::span<const int> values) {
    int total = 0;
    for (const int value : values) {
        total += value;
    }
    return total;
}
} // namespace

int main() {
    const std::array values{1, 2, 3, 4};
    const std::optional<int> total = sum(values);
    std::printf("aa-cross-smoke %d\n", total.value_or(-1));
    return total == 10 ? 0 : 1;
}
