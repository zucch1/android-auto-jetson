// SPDX-License-Identifier: GPL-3.0-or-later
#include "allocation_counter.hpp"

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

std::atomic<long long> allocations{0};

void count_allocation() noexcept { allocations.fetch_add(1, std::memory_order_relaxed); }

[[nodiscard]] void* allocate(std::size_t size) {
    count_allocation();
    void* pointer = std::malloc(size ? size : 1);
    if (!pointer) throw std::bad_alloc();
    return pointer;
}

[[nodiscard]] void* allocate_nothrow(std::size_t size) noexcept {
    count_allocation();
    return std::malloc(size ? size : 1);
}

// Over-aligned forms must be replaced too: otherwise the default aligned
// allocation functions stay in place, and an over-aligned allocation would be
// neither counted (weakening the "no retained storage" regression) nor a
// matched pair with the operators below.
[[nodiscard]] void* allocate_aligned(std::size_t size, std::align_val_t alignment) {
    count_allocation();
    void* pointer = nullptr;
    const auto align = static_cast<std::size_t>(alignment);
    if (::posix_memalign(&pointer, align, size ? size : 1) != 0) throw std::bad_alloc();
    return pointer;
}

} // namespace

long long ipctest::allocation_count() noexcept {
    return allocations.load(std::memory_order_relaxed);
}

// Replacement global allocation/deallocation functions ([new.delete]): every
// form a `new` expression can select is replaced, and each allocation is
// released by the matching replacement operator here. Memory therefore only
// ever flows malloc -> free within this pair, which is why the -O3
// -Wmismatched-new-delete diagnostic that GCC emits when these bodies are
// inlined next to a `new` expression is a false positive.
void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    return allocate_nothrow(size);
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return allocate_nothrow(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    // The nothrow contract applies to the aligned forms as well: report
    // failure instead of throwing.
    count_allocation();
    void* pointer = nullptr;
    if (::posix_memalign(&pointer, static_cast<std::size_t>(alignment), size ? size : 1) != 0) {
        return nullptr;
    }
    return pointer;
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}

void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept {
    std::free(pointer);
}
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept {
    std::free(pointer);
}
