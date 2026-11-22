// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Heap-allocation counter for the IPC framing regressions that must prove a
// malformed packet flood retains no dynamic storage.
//
// The counting replacement allocation functions are defined in
// allocation_counter.cpp, i.e. in their own translation unit, for two reasons:
// a replacement operator new/delete pair is defined exactly once for the whole
// program (never per test file), and GCC only mis-diagnoses the pairing as
// -Wmismatched-new-delete when the replacement operator delete and an inlined
// `new` expression meet inside one translation unit.
namespace ipctest {

// Total number of allocation calls made through the replacement operators
// since process start.
[[nodiscard]] long long allocation_count() noexcept;

} // namespace ipctest
