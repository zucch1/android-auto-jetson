cmake_minimum_required(VERSION 3.20)
project(android_auto_provenance LANGUAGES NONE)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Verify original provenance and stage immutable effective dependency sources.
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
include(cmake/SysrootGate.cmake)
enable_language(C CXX)
set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST "${AA_DEPENDENCY_STAGE}/googletest" CACHE PATH "Verified source" FORCE)
set(AASDK_TEST ON)
include("${AA_DEPENDENCY_STAGE}/googletest-declaration.cmake")
enable_testing()
add_test(NAME provenance_check
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/provenance/check.py"
            --root "${CMAKE_SOURCE_DIR}")
foreach(case baseline overlay content header extra generated license gpl inventory listing symlink missing credential sanitized-edit)
    add_test(NAME provenance_isolation_${case}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/provenance/scenarios.py"
                "${CMAKE_SOURCE_DIR}" "${case}")
    set_tests_properties(provenance_isolation_${case} PROPERTIES
        ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}" TIMEOUT 60)
endforeach()
include(cmake/HostSmoke.cmake)
include(cmake/TlsCredentials.cmake)
include(cmake/AasdkAbi.cmake)
include(cmake/TlsHandshake.cmake)
include(cmake/WirelessProbe.cmake)
include(cmake/UsbAoaProbe.cmake)
include(cmake/CapabilityGate.cmake)
include(cmake/CoreArchitecture.cmake)
include(cmake/ProtocolAdapter.cmake)
include(cmake/Channels.cmake)
include(cmake/Diagnostics.cmake)
include(cmake/Transport.cmake)
include(cmake/Session.cmake)
include(cmake/Replay.cmake)
include(cmake/Conformance.cmake)
include(cmake/DbusContract.cmake)
include(cmake/TrustStore.cmake)
if(TARGET aa_sysroot_hash_gate)
    # Build-time hash gate: aa_host_smoke (and staged gtest) cannot compile after a post-configure
    # sysroot tamper. Only present for jetson-aarch64 cross builds that configure a sysroot.
    add_dependencies(aa_host_smoke aa_sysroot_hash_gate)
    foreach(_aa_gtest_target gtest gtest_main gmock gmock_main)
        if(TARGET ${_aa_gtest_target})
            add_dependencies(${_aa_gtest_target} aa_sysroot_hash_gate)
        endif()
    endforeach()
    add_test(NAME cross_contamination
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/build/contamination.py"
                --binary "$<TARGET_FILE:aa_host_smoke>" --sysroot "${CMAKE_SYSROOT}"
                --build "${CMAKE_BINARY_DIR}" --source "${CMAKE_SOURCE_DIR}"
                --compiler "${CMAKE_CXX_COMPILER}" ${AA_SYSROOT_PROVENANCE_ARGS}
                --output "$<TARGET_FILE:aa_host_smoke>.inputs.json")
    target_link_options(aa_host_smoke PRIVATE "-Wl,-Map,$<TARGET_FILE:aa_host_smoke>.map")
    set_tests_properties(cross_contamination PROPERTIES TIMEOUT 60)
    add_test(NAME cross_runtime_regressions
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/build/runtime_provenance.py"
                "${CMAKE_SOURCE_DIR}" "${CMAKE_BINARY_DIR}" "${CMAKE_SYSROOT}")
    set_tests_properties(cross_runtime_regressions PROPERTIES TIMEOUT 120)
    unset(_aa_gtest_target)
endif()
add_test(NAME deps_check COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tools/deps/check_manifest.py"
    --lock "${CMAKE_SOURCE_DIR}/deps/manifest.lock" --archive "${AA_GOOGLETEST_ARCHIVE}")
foreach(case valid missing-source source-sha malformed-sha lock-digest version missing-package archive patch manifest-digest target-digest target-packages target-status)
    add_test(NAME deps_manifest_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/deps/scenarios.py" "${CMAKE_SOURCE_DIR}" "${case}" "${AA_GOOGLETEST_ARCHIVE}")
endforeach()
foreach(case reuse effective-tamper gtest-tamper extra missing-staging)
    add_test(NAME deps_stage_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/deps/staging.py" "${CMAKE_SOURCE_DIR}" "${case}" "${AA_GOOGLETEST_ARCHIVE}")
endforeach()

# Pure-python sysroot/cache tooling regressions; the two contamination cases need a cross
# compiler and stay out of the host preset (cross_contamination covers that gate).
foreach(case validate-happy validate-tamper drift-none drift-package-version
        drift-package-identity drift-package-architecture drift-same-version-content
        cache-bind cache-reuse cache-drift-same-version cache-publish cache-publish-idempotent
        cache-publish-deterministic cache-publish-tamper cache-publish-drift
        cache-publish-verify-binding-mismatch cache-publish-verify-provenance-tamper
        cache-publish-verify-version-tamper cache-bind-current-field-mismatch
        cache-bind-rebound-prior-mismatch)
    add_test(NAME build_scenario_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/build/scenarios.py" "${CMAKE_SOURCE_DIR}" "${case}")
    set_tests_properties(build_scenario_${case} PROPERTIES TIMEOUT 120)
endforeach()

# Task 5 target-safety suites: bounded, host-only, fixture-retaining under /tmp/opencode.
# Excluded suites are enumerated in tests/target_safety/assert_coverage.py with per-suite reasons;
# that coverage test fails if a suite is neither registered here nor explicitly excluded there.
set(AA_TARGET_SAFETY_SUITES
    acquire_blockers acquire_failures acquire_integrity acquire_manifest acquire_owner acquire_proc
    acquire_route acquire_transport capacity ceiling ceiling_budget ceiling_bundle
    ceiling_guard ceiling_main ceiling_production ceiling_stdin ceiling_window diag_frames
    diag_heartbeat diag_stream diag_writer envelope namespace sizing_admission
    sizing_bundle sizing_callback_timing sizing_collector sizing_completion_integrity sizing_failure_paths sizing_outcomes
    sizing_reading_validation sizing_resources sizing_rss_lifecycle sizing_selector_failures sizing_sqlite_absence sizing_sqlite_inotify
    sizing_sqlite_stdin sizing_sqlite_wire snapshot_changes snapshot_cli snapshot_diff_compare snapshot_diff_fixtures
    snapshot_diff_receipt snapshot_diff_validate snapshot_fixtures snapshot_gate_e2e snapshot_gate_fixtures snapshot_gate_review_fixes
    snapshot_git snapshot_inventory snapshot_limits snapshot_links snapshot_manifest snapshot_races
    window)
foreach(suite IN LISTS AA_TARGET_SAFETY_SUITES)
    add_test(NAME target_safety_${suite} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/target_safety/${suite}.py")
    set_tests_properties(target_safety_${suite} PROPERTIES TIMEOUT 130)
endforeach()
add_test(NAME target_safety_coverage COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/target_safety/assert_coverage.py" "${CMAKE_BINARY_DIR}")

add_subdirectory(tools/aa-ipc-bench)

# Task 9 is mandatory: missing development inputs must not turn CI green by omission.
add_subdirectory(tools/aa-decode-probe)
if(NOT CMAKE_CROSSCOMPILING)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "AA_DECODE_COVERAGE: native builds require BUILD_TESTING=ON")
    endif()
    add_test(NAME media_decode_probe_suite
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/media/test_decode_probe.py"
                "$<TARGET_FILE:aa-decode-probe>"
                "${CMAKE_SOURCE_DIR}/tests/fixtures/media/h264_720p30_baseline_90f.h264")
    set_tests_properties(media_decode_probe_suite PROPERTIES TIMEOUT 120)
endif()
