# SPDX-License-Identifier: GPL-3.0-or-later
# Task 22 production AF_UNIX video channel: the task-10 spike framing served
# from a real SOCK_SEQPACKET socket with peer-credential admission, negotiated
# limits, bounded queues, drop-to-IDR resync and replayable capture. Tests
# register as video_socket_* for `ctest -R video_socket`.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# cmake/Replay.cmake (aa_replay) and the googletest declaration.

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Video socket qualification unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_replay)
    message(FATAL_ERROR
        "AA_VIDEO_SOCKET_REQUIRES_AA_REPLAY: include cmake/Replay.cmake before cmake/VideoSocket.cmake")
endif()

set(AA_VIDEO_SOURCES
    src/ipc/video/VideoCodec.cpp
    src/ipc/video/VideoChannel.cpp
    src/ipc/video/ReplayVideoCapture.cpp)
set(AA_VIDEO_TEST_SOURCES
    tests/video_socket/video_codec_test.cpp
    tests/video_socket/video_channel_test.cpp
    tests/video_socket/video_peer_test.cpp
    tests/video_socket/video_backpressure_test.cpp
    tests/video_socket/video_malformed_test.cpp
    tests/video_socket/video_replay_test.cpp
    tests/video_socket/video_bench_test.cpp
    tests/video_socket/video_ownership_test.cpp)
foreach(source IN LISTS AA_VIDEO_SOURCES AA_VIDEO_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_VIDEO_SOURCE_MISSING: ${source} (video socket source inventory)")
    endif()
endforeach()

add_library(aa_video STATIC ${AA_VIDEO_SOURCES})
set_target_properties(aa_video PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_video PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_video PUBLIC aa_core aa_replay)

add_executable(aa_video_socket_test ${AA_VIDEO_TEST_SOURCES})
target_link_libraries(aa_video_socket_test PRIVATE aa_video aa_replay GTest::gtest_main)
set_target_properties(aa_video_socket_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

foreach(target aa_video aa_video_socket_test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_VIDEO_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_video_asan_target aa_video aa_video_socket_test)
        target_compile_options(${_aa_video_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_video_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_video_asan_target)
endif()

add_test(NAME video_socket_codec COMMAND aa_video_socket_test --gtest_filter=VideoCodec.*)
add_test(NAME video_socket_stream COMMAND aa_video_socket_test --gtest_filter=VideoStream.*)
add_test(NAME video_socket_peer COMMAND aa_video_socket_test --gtest_filter=VideoPeer.*)
add_test(NAME video_socket_backpressure COMMAND aa_video_socket_test --gtest_filter=VideoBackpressure.*)
add_test(NAME video_socket_malformed COMMAND aa_video_socket_test --gtest_filter=VideoMalformed.*)
add_test(NAME video_socket_replay COMMAND aa_video_socket_test --gtest_filter=VideoReplay.*)
add_test(NAME video_socket_bench COMMAND aa_video_socket_test --gtest_filter=VideoBench.*)
add_test(NAME video_socket_ownership COMMAND aa_video_socket_test --gtest_filter=VideoOwnership.*)
set_tests_properties(video_socket_codec video_socket_stream video_socket_peer
    video_socket_backpressure video_socket_malformed video_socket_replay video_socket_bench
    video_socket_ownership
    PROPERTIES TIMEOUT 120)

unset(AA_VIDEO_SOURCES)
unset(AA_VIDEO_TEST_SOURCES)
