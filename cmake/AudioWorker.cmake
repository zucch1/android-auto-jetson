# SPDX-License-Identifier: GPL-3.0-or-later
# Shared audio render worker, deferred AV sink registration, live-PCM drift
# config, and audio telemetry library shared by wired and wireless receivers.
#
# Integration contract: include AFTER aa_wired (cmake/WiredE2e.cmake) and
# aareceiver-audio (cmake/AudioPipeline.cmake).

if(NOT TARGET aa_wired OR NOT TARGET aareceiver-audio)
    return()
endif()

set(AA_AUDIO_WORKER_SOURCES
    src/audio/AudioRenderWorker.cpp
    src/audio/AudioTelemetry.cpp
    src/audio/AvCreditJson.cpp
    src/app/AvAudioRegistration.cpp)

foreach(source IN LISTS AA_AUDIO_WORKER_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_AUDIO_WORKER_SOURCE_MISSING: ${source}")
    endif()
endforeach()

add_library(aa_audio_worker STATIC ${AA_AUDIO_WORKER_SOURCES})
set_target_properties(aa_audio_worker PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_audio_worker PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_audio_worker PUBLIC aa_wired aareceiver-audio aa_channels aa_diagnostics)

if(TARGET PkgConfig::AA_AUDIO_GST)
    target_link_libraries(aa_audio_worker PRIVATE PkgConfig::AA_AUDIO_GST)
endif()

if(TARGET aa_sysroot_hash_gate)
    add_dependencies(aa_audio_worker aa_sysroot_hash_gate)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_audio_worker PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

if(AA_ENABLE_ASAN)
    target_compile_options(aa_audio_worker PRIVATE
        -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(aa_audio_worker PRIVATE -fsanitize=address)
endif()

unset(AA_AUDIO_WORKER_SOURCES)
