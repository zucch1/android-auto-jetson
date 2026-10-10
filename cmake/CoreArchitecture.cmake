# SPDX-License-Identifier: GPL-3.0-or-later
# Task 14 core module architecture: explicit project-owned inventory (no source
# globbing), C++20 with warnings-as-errors, and the architecture_boundary suite
# (real include scan + real temporary negative fixtures + C++ contract smoke).
set(AA_CORE_SOURCES
    src/core/Error.cpp
    src/core/Logging.cpp
    src/core/SessionThread.cpp
    src/session/StateMachine.cpp
    src/channels/Services.cpp
    src/protocol/ServiceValidation.cpp
    src/config/Validate.cpp)
set(AA_CORE_TEST_SOURCES
    tests/architecture/contract_core_test.cpp
    tests/architecture/contract_modules_test.cpp)
foreach(source IN LISTS AA_CORE_SOURCES AA_CORE_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_CORE_SOURCE_MISSING: ${source} (core source inventory)")
    endif()
endforeach()

add_library(aa_core STATIC ${AA_CORE_SOURCES})
set_target_properties(aa_core PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_core PUBLIC "${CMAKE_SOURCE_DIR}/include")
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

add_executable(aa_core_contract ${AA_CORE_TEST_SOURCES})
target_link_libraries(aa_core_contract PRIVATE aa_core GTest::gtest_main)
set_target_properties(aa_core_contract PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_core_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

option(AA_ENABLE_ASAN "Instrument project-owned host code with AddressSanitizer" OFF)
if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_asan_target aa_core aa_core_contract)
        target_compile_options(${_aa_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_asan_target)
endif()

add_test(NAME architecture_boundary_cpp_smoke COMMAND aa_core_contract)
set_tests_properties(architecture_boundary_cpp_smoke PROPERTIES TIMEOUT 60)

# Include-boundary suite: the happy case scans the actual module files; every
# negative case writes a real temporary source with a forbidden include and
# requires the scanner to fail closed on it.
set(AA_BOUNDARY_CASES
    project-scan
    neg-qt neg-gstreamer neg-bluez neg-networkmanager neg-aasdk neg-protobuf
    neg-openssl neg-whitespace neg-tls-leak neg-src-outside-adapter
    neg-qt-quoted neg-gstreamer-quoted neg-tls-quoted neg-quoted-after-comment
    allow-tls-legacy allow-src-adapter allow-commented-include)
foreach(case IN LISTS AA_BOUNDARY_CASES)
    add_test(NAME architecture_boundary_${case}
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/architecture/scenarios.py"
                "${CMAKE_SOURCE_DIR}" "${case}")
    set_tests_properties(architecture_boundary_${case} PROPERTIES TIMEOUT 60)
endforeach()
unset(case)
unset(AA_BOUNDARY_CASES)
