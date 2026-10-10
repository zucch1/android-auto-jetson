# SPDX-License-Identifier: GPL-3.0-or-later
# Task 23 consumer-side GStreamer decode helper (libaareceiver-consumer): host
# software and Jetson NVIDIA (nvv4l2decoder, runtime-detected) backends, caps
# negotiation against the channel budget profile (1280x720@30 with 800x480
# fallback), frame callbacks carrying the task-22 framing metadata, and
# aa-decode-probe integration. Tests register as consumer_decode_* for
# `ctest -R consumer_decode`.
#
# Task-9 reduced scope (owner-signed 2026-10-06): the decode p95 latency budget
# (<=33 ms) and the software-fallback qualification are WITHDRAWN claims; this
# helper and its tests record measured budgets and never claim them.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core)
# and the googletest declaration. GStreamer code lives only under
# src/consumer/gstreamer_adapter/ (architecture_boundary adapter zone).

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Consumer decode helper unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_core)
    message(FATAL_ERROR
        "AA_CONSUMER_DECODE_REQUIRES_AA_CORE: include cmake/CoreArchitecture.cmake before cmake/ConsumerDecode.cmake")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(AA_CONSUMER_GST REQUIRED IMPORTED_TARGET
    NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH
    gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)

set(AA_CONSUMER_SOURCES
    src/consumer/gstreamer_adapter/DecodeHelper.cpp)
set(AA_CONSUMER_TEST_SOURCES
    tests/consumer/consumer_decode_test.cpp
    tools/aa-decode-probe/src/discovery.cpp)
foreach(source IN LISTS AA_CONSUMER_SOURCES AA_CONSUMER_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_CONSUMER_SOURCE_MISSING: ${source} (consumer decode source inventory)")
    endif()
endforeach()

add_library(aareceiver-consumer STATIC ${AA_CONSUMER_SOURCES})
set_target_properties(aareceiver-consumer PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aareceiver-consumer PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aareceiver-consumer PUBLIC aa_core PRIVATE PkgConfig::AA_CONSUMER_GST)

add_executable(aa_consumer_decode_test ${AA_CONSUMER_TEST_SOURCES})
target_link_libraries(aa_consumer_decode_test PRIVATE
    aareceiver-consumer aa_core PkgConfig::AA_CONSUMER_GST pthread GTest::gtest_main)
set_target_properties(aa_consumer_decode_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
# The tests cross-check runtime detection against the existing aa-decode-probe
# tooling (its discovery policy is linked in and its binary is smoke-run).
target_include_directories(aa_consumer_decode_test PRIVATE
    "${CMAKE_SOURCE_DIR}/tools/aa-decode-probe/include")
target_compile_definitions(aa_consumer_decode_test PRIVATE
    AA_CONSUMER_DECODE_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/media/h264_720p30_baseline_90f.h264"
    AA_CONSUMER_DECODE_PROBE="$<TARGET_FILE:aa-decode-probe>")

foreach(target aareceiver-consumer aa_consumer_decode_test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_CONSUMER_DECODE_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_consumer_asan_target aareceiver-consumer aa_consumer_decode_test)
        target_compile_options(${_aa_consumer_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_consumer_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_consumer_asan_target)
endif()

add_test(NAME consumer_decode_caps COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeCaps.*)
add_test(NAME consumer_decode_fixture COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeFixture.*)
add_test(NAME consumer_decode_backend COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeBackend.*)
add_test(NAME consumer_decode_probe COMMAND aa_consumer_decode_test --gtest_filter=ConsumerProbeIntegration.*)
set_tests_properties(consumer_decode_caps consumer_decode_fixture consumer_decode_backend
    consumer_decode_probe
    PROPERTIES TIMEOUT 120)

unset(AA_CONSUMER_SOURCES)
unset(AA_CONSUMER_TEST_SOURCES)
