# SPDX-License-Identifier: GPL-3.0-or-later
# Task 19 structured diagnostics + privacy redaction.
#
# Integration contract: include this AFTER cmake/CoreArchitecture.cmake (which
# defines the aa_core target holding redact_identifier) and AFTER the googletest
# declaration (for GTest::gtest_main) and Python3. Do NOT edit root
# CMakeLists.txt or CoreArchitecture.cmake to wire this module; the parent adds
# `include(cmake/Diagnostics.cmake)` in its own root edit.
#
# AA_REPO_ROOT defaults to CMAKE_SOURCE_DIR so the parent (top-level = repo
# root) needs no extra setup; a standalone/verification project may override it.

if(NOT DEFINED AA_REPO_ROOT)
    set(AA_REPO_ROOT "${CMAKE_SOURCE_DIR}")
endif()
if(NOT TARGET aa_core)
    message(FATAL_ERROR
        "AA_DIAGNOSTICS_REQUIRES_AA_CORE: include cmake/CoreArchitecture.cmake before cmake/Diagnostics.cmake")
endif()

set(AA_DIAGNOSTICS_REL_SOURCES
    src/diagnostics/Policy.cpp
    src/diagnostics/ValuePolicy.cpp
    src/diagnostics/Scrub.cpp
    src/diagnostics/Serialize.cpp
    src/diagnostics/SafeLogSink.cpp)
set(AA_DIAGNOSTICS_REL_TEST_SOURCES
    tests/diagnostics/redaction_test.cpp
    tests/diagnostics/privacy_gaps_test.cpp
    tests/diagnostics/emission_schema_test.cpp)
set(AA_DIAGNOSTICS_REL_PROBE_SOURCES
    tests/diagnostics/json_probe.cpp)
set(AA_DIAGNOSTICS_SOURCES "")
set(AA_DIAGNOSTICS_TEST_SOURCES "")
set(AA_DIAGNOSTICS_PROBE_SOURCES "")
foreach(source IN LISTS AA_DIAGNOSTICS_REL_SOURCES)
    if(NOT EXISTS "${AA_REPO_ROOT}/${source}")
        message(FATAL_ERROR "AA_DIAGNOSTICS_SOURCE_MISSING: ${source} (diagnostics source inventory)")
    endif()
    list(APPEND AA_DIAGNOSTICS_SOURCES "${AA_REPO_ROOT}/${source}")
endforeach()
foreach(source IN LISTS AA_DIAGNOSTICS_REL_TEST_SOURCES)
    if(NOT EXISTS "${AA_REPO_ROOT}/${source}")
        message(FATAL_ERROR "AA_DIAGNOSTICS_SOURCE_MISSING: ${source} (diagnostics source inventory)")
    endif()
    list(APPEND AA_DIAGNOSTICS_TEST_SOURCES "${AA_REPO_ROOT}/${source}")
endforeach()
foreach(source IN LISTS AA_DIAGNOSTICS_REL_PROBE_SOURCES)
    if(NOT EXISTS "${AA_REPO_ROOT}/${source}")
        message(FATAL_ERROR "AA_DIAGNOSTICS_SOURCE_MISSING: ${source} (diagnostics source inventory)")
    endif()
    list(APPEND AA_DIAGNOSTICS_PROBE_SOURCES "${AA_REPO_ROOT}/${source}")
endforeach()

# Serializer/redaction boundary. Links aa_core for the redact_identifier
# pseudonymizer (single definition lives in aa_core's src/diagnostics/Redaction.cpp).
add_library(aa_diagnostics STATIC ${AA_DIAGNOSTICS_SOURCES})
target_link_libraries(aa_diagnostics PUBLIC aa_core)
set_target_properties(aa_diagnostics PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_diagnostics PUBLIC "${AA_REPO_ROOT}/include")
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_diagnostics PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

add_executable(aa_diagnostics_contract ${AA_DIAGNOSTICS_TEST_SOURCES})
target_link_libraries(aa_diagnostics_contract PRIVATE aa_diagnostics GTest::gtest_main)
set_target_properties(aa_diagnostics_contract PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_diagnostics_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

# Test-only JSON probe: emits redacted fixture JSON for the Python parser check.
add_executable(aa_diagnostics_json_probe ${AA_DIAGNOSTICS_PROBE_SOURCES})
target_link_libraries(aa_diagnostics_json_probe PRIVATE aa_diagnostics)
set_target_properties(aa_diagnostics_json_probe PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_diagnostics_json_probe PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

option(AA_ENABLE_ASAN "Instrument project-owned host code with AddressSanitizer" OFF)
if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_diag_asan_target aa_diagnostics aa_diagnostics_contract aa_diagnostics_json_probe)
        target_compile_options(${_aa_diag_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_diag_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_diag_asan_target)
endif()

# ctest -R diagnostics_redaction runs both: the C++ privacy/escaping contract and
# the authoritative real-parser JSON validity + no-leak check.
add_test(NAME diagnostics_redaction_contract COMMAND aa_diagnostics_contract)
set_tests_properties(diagnostics_redaction_contract PROPERTIES TIMEOUT 60)

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
add_test(NAME diagnostics_redaction_json
    COMMAND "${Python3_EXECUTABLE}" -B
            "${AA_REPO_ROOT}/tests/diagnostics/test_json.py"
            "$<TARGET_FILE:aa_diagnostics_json_probe>")
set_tests_properties(diagnostics_redaction_json PROPERTIES TIMEOUT 60)
