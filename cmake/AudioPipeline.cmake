# SPDX-License-Identifier: GPL-3.0-or-later
# Task 25 AAP audio decode + PipeWire routing: four channels (media, guidance,
# system, speech) decoded from the channel/session audio path, duck/mixed with
# documented ratios, routed to the current default PipeWire output through the
# runtime-probed routing elements (pipewiresink / pulsesink / recorded-disabled).
# Microphone seam is fixture-tested and disabled by default. Tests register as
# audio_pipeline_* for `ctest -R audio_pipeline`.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core)
# and the googletest declaration. GStreamer code lives only under
# src/audio/gstreamer_adapter/ (architecture_boundary adapter zone).

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Audio pipeline unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_core)
    message(FATAL_ERROR
        "AA_AUDIO_REQUIRES_AA_CORE: include cmake/CoreArchitecture.cmake before cmake/AudioPipeline.cmake")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(AA_AUDIO_GST REQUIRED IMPORTED_TARGET
    NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH
    gstreamer-1.0 gstreamer-app-1.0)

set(AA_AUDIO_SOURCES
    src/audio/AudioMix.cpp
    src/audio/AudioRouter.cpp
    src/audio/gstreamer_adapter/GstAudioDecoder.cpp
    src/audio/gstreamer_adapter/GstAudioSink.cpp)
set(AA_AUDIO_TEST_SOURCES
    tests/audio/audio_pipeline_test.cpp)
foreach(source IN LISTS AA_AUDIO_SOURCES AA_AUDIO_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_AUDIO_SOURCE_MISSING: ${source} (audio pipeline source inventory)")
    endif()
endforeach()

add_library(aareceiver-audio STATIC ${AA_AUDIO_SOURCES})
set_target_properties(aareceiver-audio PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aareceiver-audio PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aareceiver-audio PUBLIC aa_core PRIVATE PkgConfig::AA_AUDIO_GST)

add_executable(aa_audio_pipeline_test ${AA_AUDIO_TEST_SOURCES})
target_link_libraries(aa_audio_pipeline_test PRIVATE
    aareceiver-audio aa_core PkgConfig::AA_AUDIO_GST pthread GTest::gtest_main)
set_target_properties(aa_audio_pipeline_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_definitions(aa_audio_pipeline_test PRIVATE
    AA_AUDIO_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/tests/fixtures/audio")

foreach(target aareceiver-audio aa_audio_pipeline_test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_AUDIO_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_audio_asan_target aareceiver-audio aa_audio_pipeline_test)
        target_compile_options(${_aa_audio_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_audio_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_audio_asan_target)
endif()

add_test(NAME audio_pipeline_mix COMMAND aa_audio_pipeline_test --gtest_filter=AudioPipelineMix.*)
add_test(NAME audio_pipeline_route COMMAND aa_audio_pipeline_test --gtest_filter=AudioPipelineRoute.*)
add_test(NAME audio_pipeline_decode COMMAND aa_audio_pipeline_test --gtest_filter=AudioPipelineDecodeSuite.*)
add_test(NAME audio_pipeline_sink COMMAND aa_audio_pipeline_test --gtest_filter=AudioPipelineSinkBackend.*)
add_test(NAME audio_pipeline_mic COMMAND aa_audio_pipeline_test --gtest_filter=MicrophoneContract.*)
set_tests_properties(audio_pipeline_mix audio_pipeline_route audio_pipeline_decode
    audio_pipeline_sink audio_pipeline_mic
    PROPERTIES TIMEOUT 120)

unset(AA_AUDIO_SOURCES)
unset(AA_AUDIO_TEST_SOURCES)
