# SPDX-License-Identifier: GPL-3.0-or-later
# Task 24 display-input channel (owner ruling 2026-10-10,
# .omo/evidence/jetson-android-auto-receiver/execution-input-transport-decision.json):
# a NEW bounded AF_UNIX SOCK_SEQPACKET socket in the task-22 surface family,
# served from input.sock beside video.sock. The task-21 D-Bus contract stays
# control-only - this socket is the input transport. Tests register as
# input_socket_* for `ctest -R input_socket`.
#
# aa_input is production code and compiles in every configuration (the
# cross-build IPC inventory requires its sources in compile_commands.json);
# only the test executable and its registrations stay behind
# if(NOT CMAKE_CROSSCOMPILING).
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# cmake/VideoSocket.cmake (aa_video) and the googletest declaration.

if(NOT TARGET aa_video)
    message(FATAL_ERROR
        "AA_INPUT_SOCKET_REQUIRES_AA_VIDEO: include cmake/VideoSocket.cmake before cmake/InputSocket.cmake")
endif()

set(AA_INPUT_SOURCES
    src/ipc/input/InputCodec.cpp
    src/ipc/input/InputChannel.cpp)
set(AA_INPUT_TEST_SOURCES
    tests/input_socket/input_channel_test.cpp
    tests/input_socket/input_peer_test.cpp
    tests/input_socket/input_malformed_test.cpp)
foreach(source IN LISTS AA_INPUT_SOURCES AA_INPUT_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_INPUT_SOURCE_MISSING: ${source} (input socket source inventory)")
    endif()
endforeach()

add_library(aa_input STATIC ${AA_INPUT_SOURCES})
set_target_properties(aa_input PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_input PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_input PUBLIC aa_core aa_video)

if(TARGET aa_sysroot_hash_gate)
    # Root hash-gate convention: this library cannot compile after a
    # post-configure sysroot tamper (present for jetson-aarch64 cross builds).
    add_dependencies(aa_input aa_sysroot_hash_gate)
endif()

set(AA_INPUT_WARN_TARGETS aa_input)
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa_input_socket_test ${AA_INPUT_TEST_SOURCES})
    target_link_libraries(aa_input_socket_test PRIVATE aa_input aa_video aa_core GTest::gtest_main)
    set_target_properties(aa_input_socket_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    list(APPEND AA_INPUT_WARN_TARGETS aa_input_socket_test)
endif()

foreach(target IN LISTS AA_INPUT_WARN_TARGETS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_INPUT_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_input_asan_target IN LISTS AA_INPUT_WARN_TARGETS)
        target_compile_options(${_aa_input_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_input_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_input_asan_target)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME input_socket_channel COMMAND aa_input_socket_test --gtest_filter=InputChannel.*)
    add_test(NAME input_socket_peer COMMAND aa_input_socket_test --gtest_filter=InputPeer.*)
    add_test(NAME input_socket_malformed COMMAND aa_input_socket_test --gtest_filter=InputMalformed.*)
    set_tests_properties(input_socket_channel input_socket_peer input_socket_malformed
        PROPERTIES TIMEOUT 120)
endif()

unset(AA_INPUT_SOURCES)
unset(AA_INPUT_TEST_SOURCES)
unset(AA_INPUT_WARN_TARGETS)
