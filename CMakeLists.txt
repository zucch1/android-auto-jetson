# Task-3 delivery wiring: dependency pinning and offline staging on top of the
# task-2 provenance gate. 29 CTest registrations (13 provenance + 1 manifest
# check + 10 manifest mutations + 5 staging cases).
# Delivery-derived file (written for this task-3 delivery snapshot); the
# historical task-3 CMake bytes are unavailable and were not recovered. It is
# not original upstream history content. Host smoke/preset wiring is owned by
# task 4 and intentionally excluded (no include(cmake/HostSmoke.cmake), no host
# preset fallback bits).
cmake_minimum_required(VERSION 3.20)
project(android_auto_provenance LANGUAGES NONE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Dependency-only configure: full AASDK ABI qualification belongs to task 7.
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
set(AA_GOOGLETEST_ARCHIVE "$ENV{AA_GOOGLETEST_ARCHIVE}" CACHE FILEPATH "Explicit pre-staged GoogleTest v1.15.2 archive")
if(NOT AA_GOOGLETEST_ARCHIVE)
    set(AA_GOOGLETEST_ARCHIVE "${CMAKE_SOURCE_DIR}/.local/dependencies/googletest-v1.15.2.tar.gz")
endif()
if(NOT AA_GOOGLETEST_ARCHIVE OR NOT EXISTS "${AA_GOOGLETEST_ARCHIVE}")
    message(FATAL_ERROR "AA_GOOGLETEST_ARCHIVE must name a pre-staged archive; configure never downloads")
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/deps/check_manifest.py"
            --lock "${CMAKE_SOURCE_DIR}/deps/manifest.lock" --archive "${AA_GOOGLETEST_ARCHIVE}"
    RESULT_VARIABLE manifest_result OUTPUT_VARIABLE manifest_output ERROR_VARIABLE manifest_error)
if(NOT manifest_result EQUAL 0)
    message(FATAL_ERROR "Dependency lock verification failed: ${manifest_output}${manifest_error}")
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/deps/stage.py"
            --root "${CMAKE_SOURCE_DIR}" --cache "${CMAKE_BINARY_DIR}/deps"
            --archive "${AA_GOOGLETEST_ARCHIVE}"
    RESULT_VARIABLE stage_result OUTPUT_VARIABLE AA_DEPENDENCY_STAGE
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE stage_error)
if(NOT stage_result EQUAL 0)
    message(FATAL_ERROR "Dependency staging failed: ${stage_error}")
endif()
# Execute the actual dependency block extracted from patched effective AASDK.
enable_language(C CXX)
set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${AA_DEPENDENCY_STAGE}/googletest" CACHE PATH "Verified source" FORCE)
set(AASDK_TEST ON)
include("${AA_DEPENDENCY_STAGE}/googletest-declaration.cmake")
enable_testing()
add_test(NAME provenance_check
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/provenance/check.py"
            --root "${CMAKE_SOURCE_DIR}")
foreach(case baseline overlay content header extra generated license gpl inventory listing symlink missing)
    add_test(NAME provenance_isolation_${case}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/provenance/scenarios.py"
                "${CMAKE_SOURCE_DIR}" "${case}")
    set_tests_properties(provenance_isolation_${case} PROPERTIES
        ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}" TIMEOUT 60)
endforeach()
add_test(NAME deps_check COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tools/deps/check_manifest.py"
    --lock "${CMAKE_SOURCE_DIR}/deps/manifest.lock" --archive "${AA_GOOGLETEST_ARCHIVE}")
foreach(case valid missing-source source-sha malformed-sha lock-digest version missing-package archive patch manifest-digest)
    add_test(NAME deps_manifest_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/deps/scenarios.py" "${CMAKE_SOURCE_DIR}" "${case}" "${AA_GOOGLETEST_ARCHIVE}")
endforeach()
foreach(case reuse effective-tamper gtest-tamper extra missing-staging)
    add_test(NAME deps_stage_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/deps/staging.py" "${CMAKE_SOURCE_DIR}" "${case}" "${AA_GOOGLETEST_ARCHIVE}")
endforeach()
