cmake_minimum_required(VERSION 3.20)
project(aa_decode_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/DecodeDependencies.cmake")
if(NOT AA_DECODE_PROBE_READY)
    # Cross configure where target GStreamer dev metadata is absent: the probe is explicitly
    # UNAVAILABLE (status already reported by DecodeDependencies.cmake). Register nothing:
    # no probe targets, no probe tests. Target qualification builds run in the Jetson window.
    message(STATUS "aa-decode-probe: no probe targets or tests registered")
    return()
endif()

add_executable(aa-decode-probe
    src/main.cpp
    src/options.cpp
    src/discovery.cpp
    src/metrics.cpp
    src/pipeline.cpp
    src/json_report.cpp
)

target_include_directories(aa-decode-probe PRIVATE include)
target_link_libraries(aa-decode-probe PRIVATE PkgConfig::GST pthread)

if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    target_compile_options(aa-decode-probe PRIVATE
        -Wall -Wextra -Wpedantic -Werror
    )
endif()

include(CTest)
set(_aa_decode_targets aa-decode-probe)
if(BUILD_TESTING)
    add_executable(decode-probe-inputs ../../tests/media/decode_probe_inputs.cpp src/options.cpp src/json_report.cpp)
    add_executable(decode-probe-metrics ../../tests/media/decode_probe_metrics.cpp src/metrics.cpp)
    add_executable(decode-probe-discovery ../../tests/media/decode_probe_discovery.cpp src/discovery.cpp)
    target_link_libraries(decode-probe-discovery PRIVATE PkgConfig::GST)
    foreach(test_target decode-probe-inputs decode-probe-metrics decode-probe-discovery)
        list(APPEND _aa_decode_targets ${test_target})
        target_include_directories(${test_target} PRIVATE include)
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME ${test_target} COMMAND ${test_target})
            set_tests_properties(${test_target} PROPERTIES TIMEOUT 10)
        endif()
    endforeach()
endif()
if(TARGET aa_sysroot_hash_gate)
    foreach(_aa_decode_target IN LISTS _aa_decode_targets)
        # Ninja completes the gate's custom command before any source compilation.
        add_dependencies(${_aa_decode_target} aa_sysroot_hash_gate)
        target_link_options(${_aa_decode_target} PRIVATE "-Wl,-Map,$<TARGET_FILE:${_aa_decode_target}>.map")
        add_test(NAME cross_contamination_${_aa_decode_target}
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/../build/contamination.py"
                --binary "$<TARGET_FILE:${_aa_decode_target}>" --sysroot "${CMAKE_SYSROOT}"
                --build "${CMAKE_BINARY_DIR}" --source "${CMAKE_SOURCE_DIR}"
                --compiler "${CMAKE_CXX_COMPILER}" ${AA_SYSROOT_PROVENANCE_ARGS}
                --output "$<TARGET_FILE:${_aa_decode_target}>.inputs.json")
        set_tests_properties(cross_contamination_${_aa_decode_target} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
if(BUILD_TESTING AND NOT CMAKE_CROSSCOMPILING)
    foreach(_aa_policy_case isolated host-path missing escape symlink unbound ordering partial overlay overlay-escape overlay-poison overlay-partial overlay-comma)
        add_test(NAME decode-build-policy-${_aa_policy_case}
            COMMAND "${CMAKE_COMMAND}" "-DROOT=${CMAKE_CURRENT_SOURCE_DIR}/../.."
                "-DSCRATCH=${CMAKE_CURRENT_BINARY_DIR}/policy-${_aa_policy_case}"
                "-DCASE=${_aa_policy_case}" -P "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/media/decode_build_policy.cmake")
        set_tests_properties(decode-build-policy-${_aa_policy_case} PROPERTIES TIMEOUT 60)
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME decode-probe-smoke COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/media/test_decode_probe.py
        $<TARGET_FILE:aa-decode-probe>
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/media/h264_720p30_baseline_90f.h264)
    set_tests_properties(decode-probe-smoke PROPERTIES TIMEOUT 45)
    add_test(NAME decode-probe-corrections COMMAND ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/media/test_decode_probe_corrections.py
        $<TARGET_FILE:aa-decode-probe>
        ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fixtures/media/h264_720p30_baseline_90f.h264)
    set_tests_properties(decode-probe-corrections PROPERTIES TIMEOUT 45)
endif()
