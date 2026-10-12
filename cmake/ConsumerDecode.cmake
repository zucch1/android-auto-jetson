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
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# the googletest declaration AND add_subdirectory(tools/aa-decode-probe).
# GStreamer code lives only under src/consumer/gstreamer_adapter/
# (architecture_boundary adapter zone).
#
# Cross builds compile the production library whenever target GStreamer
# development metadata is available: cmake/DecodeDependencies.cmake (loaded by
# the decode-probe subproject) binds pkg-config to the hash-validated sysroot
# payload or the explicit AA_DECODE_GST_OVERLAY capture root and never consults
# host metadata. With no target metadata the helper is explicitly UNAVAILABLE
# and no library is compiled. Only test executables, registrations and the
# aa-decode-probe tooling references are host-side.

if(CMAKE_CROSSCOMPILING)
    # AA_DECODE_PROBE_CROSS is set for every cross configuration by
    # cmake/DecodeDependencies.cmake: AVAILABLE (sanctioned target metadata
    # bound to pkg-config) or UNAVAILABLE (no target GStreamer dev metadata in
    # the frozen sysroot or the overlay). The library cannot compile without
    # target headers, so absent metadata is an explicit unavailability, never a
    # silent skip.
    if(NOT AA_DECODE_PROBE_CROSS STREQUAL "AVAILABLE")
        message(STATUS "Consumer decode helper unavailable in cross-smoke mode (no target GStreamer dev metadata)")
        return()
    endif()
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
# FindPkgConfig leaves -l inputs as bare names when a root-only toolchain
# re-roots the overlay libdir out of find_library's reach, and its imported
# target does not carry the reported -L dirs. Re-attach them so executables
# linking this static lib can resolve its private GStreamer deps.
if(AA_CONSUMER_GST_LIBRARY_DIRS)
    target_link_directories(aareceiver-consumer PUBLIC ${AA_CONSUMER_GST_LIBRARY_DIRS})
endif()
# Build-time provenance gates: a direct target build after a cross configure must
# revalidate the hash-gated sysroot/overlay inputs before compiling. Each guard
# is independent - a gate is attached on its own presence, never inferred from the
# other (mirrors tools/aa-decode-probe/CMakeLists.txt).
if(TARGET aa_sysroot_hash_gate)
    add_dependencies(aareceiver-consumer aa_sysroot_hash_gate)
endif()
if(TARGET aa_decode_overlay_hash_gate)
    add_dependencies(aareceiver-consumer aa_decode_overlay_hash_gate)
endif()

set(AA_CONSUMER_WARN_TARGETS aareceiver-consumer)
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa_consumer_decode_test ${AA_CONSUMER_TEST_SOURCES})
    target_link_libraries(aa_consumer_decode_test PRIVATE
        aareceiver-consumer aa_core PkgConfig::AA_CONSUMER_GST pthread GTest::gtest_main)
    set_target_properties(aa_consumer_decode_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    # The tests cross-check runtime detection against the existing aa-decode-probe
    # tooling (its discovery policy is linked in and its binary is smoke-run),
    # and drive the GStreamer-free admission state helper directly.
    target_include_directories(aa_consumer_decode_test PRIVATE
        "${CMAKE_SOURCE_DIR}/tools/aa-decode-probe/include"
        "${CMAKE_SOURCE_DIR}/src/consumer/gstreamer_adapter")
    target_compile_definitions(aa_consumer_decode_test PRIVATE
        AA_CONSUMER_DECODE_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/media/h264_720p30_baseline_90f.h264"
        AA_CONSUMER_DECODE_PROBE="$<TARGET_FILE:aa-decode-probe>")
    list(APPEND AA_CONSUMER_WARN_TARGETS aa_consumer_decode_test)
endif()

foreach(target IN LISTS AA_CONSUMER_WARN_TARGETS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_CONSUMER_DECODE_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_consumer_asan_target IN LISTS AA_CONSUMER_WARN_TARGETS)
        target_compile_options(${_aa_consumer_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_consumer_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_consumer_asan_target)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME consumer_decode_caps COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeCaps.*)
    add_test(NAME consumer_decode_fixture COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeFixture.*)
    add_test(NAME consumer_decode_backend COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeBackend.*)
    add_test(NAME consumer_decode_probe COMMAND aa_consumer_decode_test --gtest_filter=ConsumerProbeIntegration.*)
    add_test(NAME consumer_decode_admission COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeAdmission.*)
    add_test(NAME consumer_decode_runtime
    COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeRuntime.*)
add_test(NAME consumer_decode_correlation COMMAND aa_consumer_decode_test --gtest_filter=ConsumerDecodeCorrelation.*)
    set_tests_properties(consumer_decode_caps consumer_decode_fixture consumer_decode_backend
        consumer_decode_probe consumer_decode_admission consumer_decode_correlation
        consumer_decode_runtime PROPERTIES TIMEOUT 120)
    # Host-only tamper regression drivers: each configures the actual root project
    # for cross against the fixture-only hash-gated sysroot/overlay pair, changes a
    # fixture input AFTER configure, and proves the direct aareceiver-consumer build
    # re-runs the provenance gates before compiling. Synthetic compile/build-policy
    # fixtures, not target qualification. Generous timeout: they configure + build.
    foreach(_aa_consumer_tamper_case sysroot overlay)
        add_test(NAME consumer_decode_build_${_aa_consumer_tamper_case}_tamper
            COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/consumer/consumer_decode_build_policy.py"
                --case "${_aa_consumer_tamper_case}"
                --root "${CMAKE_SOURCE_DIR}"
                --archive "${AA_GOOGLETEST_ARCHIVE}"
                --logs "${CMAKE_CURRENT_BINARY_DIR}/consumer-decode-tamper")
        set_tests_properties(consumer_decode_build_${_aa_consumer_tamper_case}_tamper
            PROPERTIES TIMEOUT 600)
    endforeach()
    unset(_aa_consumer_tamper_case)
endif()

unset(AA_CONSUMER_SOURCES)
unset(AA_CONSUMER_TEST_SOURCES)
unset(AA_CONSUMER_WARN_TARGETS)
