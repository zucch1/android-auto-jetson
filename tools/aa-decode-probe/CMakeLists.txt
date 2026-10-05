cmake_minimum_required(VERSION 3.20)
project(aa_decode_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)

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
if(BUILD_TESTING)
    add_executable(decode-probe-inputs ../../tests/media/decode_probe_inputs.cpp src/options.cpp src/json_report.cpp)
    add_executable(decode-probe-metrics ../../tests/media/decode_probe_metrics.cpp src/metrics.cpp)
    add_executable(decode-probe-discovery ../../tests/media/decode_probe_discovery.cpp src/discovery.cpp)
    target_link_libraries(decode-probe-discovery PRIVATE PkgConfig::GST)
    foreach(test_target decode-probe-inputs decode-probe-metrics decode-probe-discovery)
        target_include_directories(${test_target} PRIVATE include)
        target_compile_options(${test_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        add_test(NAME ${test_target} COMMAND ${test_target})
        set_tests_properties(${test_target} PROPERTIES TIMEOUT 10)
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
