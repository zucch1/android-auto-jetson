// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <span>

#include <gtest/gtest.h>

namespace {
TEST(HostCpp20, SortsOnlyTheSelectedSpan) {
    // Given: a bounded interior view with sentinel values outside it.
    std::array values{99, 4, -2, 4, 1, 88};
    const auto selected = std::span{values}.subspan(1, 4);
    // When: the C++20 ranges algorithm sorts that view.
    std::ranges::sort(selected);
    // Then: duplicates and negatives sort; the sentinels are untouched.
    const std::array expected{99, -2, 1, 4, 4, 88};
    EXPECT_EQ(values, expected);
}

TEST(HostCpp20, AcceptsAnEmptySpan) {
    // Given: an empty bounded view.
    std::array<int, 0> values{};
    const auto selected = std::span{values};
    // When: the same algorithm receives no elements.
    std::ranges::sort(selected);
    // Then: the result remains empty.
    EXPECT_TRUE(selected.empty());
}
} // namespace
