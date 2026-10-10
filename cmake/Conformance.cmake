# SPDX-License-Identifier: GPL-3.0-or-later
# Pre-28 wire-conformance qualification (decision-oaa-aasdk-conformance.json
# action items 3 and 5): independent raw-byte fixtures (tests/replay/
# conformance_wire_spec.hpp) bound to the task-20 replay harness, plus the
# extension-tolerance separation tests for the project-owned decoding path.
# Tests register as replay_conformance / conformance_tolerance so both
# `ctest -R replay` and `ctest -R conformance` select them. Additive: no
# accepted task 1-20 test registration is modified.
#
# Integration contract: include AFTER cmake/Replay.cmake (aa_replay,
# aa_protocol_adapter, aap_protobuf, aasdk) and the googletest declaration.

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Conformance qualification unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_replay)
    message(FATAL_ERROR
        "AA_CONFORMANCE_REQUIRES_AA_REPLAY: include cmake/Replay.cmake before cmake/Conformance.cmake")
endif()

set(AA_CONFORMANCE_TEST_SOURCES
    tests/replay/replay_conformance_test.cpp
    tests/protocol/unknown_field_tolerance_test.cpp)
set(AA_CONFORMANCE_WRITE_SOURCES
    tests/replay/conformance_fixture_write.cpp)
foreach(source IN LISTS AA_CONFORMANCE_TEST_SOURCES AA_CONFORMANCE_WRITE_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_CONFORMANCE_SOURCE_MISSING: ${source} (conformance source inventory)")
    endif()
endforeach()

add_executable(aa_conformance_test ${AA_CONFORMANCE_TEST_SOURCES})
target_link_libraries(aa_conformance_test PRIVATE
    aa_replay aa_protocol_adapter aap_protobuf aasdk GTest::gtest_main)
target_compile_definitions(aa_conformance_test PRIVATE
    AA_CONFORMANCE_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/replay/fixtures/conformance")
set_target_properties(aa_conformance_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

add_executable(aa_conformance_fixture_write ${AA_CONFORMANCE_WRITE_SOURCES})
target_link_libraries(aa_conformance_fixture_write PRIVATE
    aa_replay aa_protocol_adapter aap_protobuf aasdk)
set_target_properties(aa_conformance_fixture_write PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

foreach(target aa_conformance_test aa_conformance_fixture_write)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

add_test(NAME replay_conformance
    COMMAND aa_conformance_test --gtest_filter=ReplayConformance.*)
add_test(NAME conformance_tolerance
    COMMAND aa_conformance_test --gtest_filter=ConformanceTolerance.*)
set_tests_properties(replay_conformance conformance_tolerance PROPERTIES TIMEOUT 60)

unset(AA_CONFORMANCE_TEST_SOURCES)
unset(AA_CONFORMANCE_WRITE_SOURCES)
