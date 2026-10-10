# SPDX-License-Identifier: GPL-3.0-or-later
# Task 20 capture/replay fixture framework: one bounded recorder, one replay
# player, a versioned fixture schema with a SHA-256 gate over fixed canonical
# bytes, the metadata redaction/export gate (task-19 to_json boundary plus
# fixed redaction), and deterministic real-core replay tests for
# transport/session/video/audio/input. Tests register as replay_* for
# `ctest -R replay`; aa_replay_fixture_probe is the manually rerunnable driver
# behind tests/replay/test_fixture_qa.py.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# cmake/ProtocolAdapter.cmake (aa_protocol_adapter), cmake/Channels.cmake,
# cmake/Diagnostics.cmake, cmake/Transport.cmake and cmake/Session.cmake, and
# after the googletest declaration (GTest::gtest_main) and Python3.

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Replay qualification unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_session)
    message(FATAL_ERROR
        "AA_REPLAY_REQUIRES_AA_SESSION: include cmake/Session.cmake before cmake/Replay.cmake")
endif()

set(AA_REPLAY_SOURCES
    src/replay/Sha256.cpp
    src/replay/Json.cpp
    src/replay/JsonLex.cpp
    src/replay/JsonParser.cpp
    src/replay/MetadataGate.cpp
    src/replay/FixtureVocab.cpp
    src/replay/FixtureBudget.cpp
    src/replay/FixtureTyped.cpp
    src/replay/FixtureValidate.cpp
    src/replay/FixtureWrite.cpp
    src/replay/FixtureEncode.cpp
    src/replay/FixtureServices.cpp
    src/replay/FixtureSession.cpp
    src/replay/FixtureRecords.cpp
    src/replay/FixtureDecode.cpp
    src/replay/Recorder.cpp
    src/replay/ReplayTransport.cpp
    src/replay/PlayerControl.cpp
    src/replay/Player.cpp)
set(AA_REPLAY_TEST_SOURCES
    tests/replay/replay_trace_test.cpp
    tests/replay/replay_bounds_test.cpp
    tests/replay/replay_golden_test.cpp
    tests/replay/replay_recorder_test.cpp
    tests/replay/replay_syntax_test.cpp
    tests/replay/replay_verdict_test.cpp
    tests/replay/replay_teardown_test.cpp
    tests/replay/replay_cancel_test.cpp
    tests/replay/replay_transport_test.cpp
    tests/replay/replay_typed_test.cpp
    tests/replay/replay_parity_test.cpp)
set(AA_REPLAY_PROBE_SOURCES
    tests/replay/fixture_probe.cpp)
foreach(source IN LISTS AA_REPLAY_SOURCES AA_REPLAY_TEST_SOURCES AA_REPLAY_PROBE_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_REPLAY_SOURCE_MISSING: ${source} (replay source inventory)")
    endif()
endforeach()

add_library(aa_replay STATIC ${AA_REPLAY_SOURCES})
set_target_properties(aa_replay PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_replay PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_replay PUBLIC aa_core aa_session aa_channels aa_transport aa_diagnostics
    aa_protocol_adapter)

add_executable(aa_replay_test ${AA_REPLAY_TEST_SOURCES})
target_link_libraries(aa_replay_test PRIVATE
    aa_replay aa_protocol_adapter aap_protobuf aasdk GTest::gtest_main)
set_target_properties(aa_replay_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

add_executable(aa_replay_fixture_probe ${AA_REPLAY_PROBE_SOURCES})
target_link_libraries(aa_replay_fixture_probe PRIVATE
    aa_replay aa_protocol_adapter aap_protobuf aasdk)
set_target_properties(aa_replay_fixture_probe PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

foreach(target aa_replay aa_replay_test aa_replay_fixture_probe)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_REPLAY_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_replay_asan_target aa_replay aa_replay_test aa_replay_fixture_probe)
        target_compile_options(${_aa_replay_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_replay_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_replay_asan_target)
endif()

add_test(NAME replay_trace COMMAND aa_replay_test --gtest_filter=ReplayTrace.*)
add_test(NAME replay_lifecycle COMMAND aa_replay_test --gtest_filter=ReplayLifecycle.*)
add_test(NAME replay_transport COMMAND aa_replay_test --gtest_filter=ReplayTransport.*)
add_test(NAME replay_bounds COMMAND aa_replay_test --gtest_filter=ReplayBounds.*)
add_test(NAME replay_golden COMMAND aa_replay_test --gtest_filter=ReplayGolden.*)
add_test(NAME replay_recorder COMMAND aa_replay_test --gtest_filter=ReplayRecorder.*)
add_test(NAME replay_syntax COMMAND aa_replay_test --gtest_filter=ReplaySyntax.*)
add_test(NAME replay_verdict COMMAND aa_replay_test --gtest_filter=ReplayVerdict.*)
add_test(NAME replay_teardown COMMAND aa_replay_test --gtest_filter=ReplayTeardown.*)
add_test(NAME replay_cancel COMMAND aa_replay_test --gtest_filter=ReplayCancel.*)
add_test(NAME replay_typed COMMAND aa_replay_test --gtest_filter=ReplayTyped.*)
add_test(NAME replay_parity COMMAND aa_replay_test --gtest_filter=ReplayParity.*)
set_tests_properties(replay_trace replay_lifecycle replay_transport replay_bounds replay_golden
    replay_recorder replay_syntax replay_verdict replay_teardown replay_cancel replay_typed replay_parity
    PROPERTIES TIMEOUT 60)

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_test(NAME replay_qa
    COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/replay/test_fixture_qa.py"
            "$<TARGET_FILE:aa_replay_fixture_probe>"
            "${CMAKE_SOURCE_DIR}/tests/fixtures/media/h264_720p30_baseline_90f.h264")
set_tests_properties(replay_qa PROPERTIES TIMEOUT 60)

unset(AA_REPLAY_SOURCES)
unset(AA_REPLAY_TEST_SOURCES)
unset(AA_REPLAY_PROBE_SOURCES)
