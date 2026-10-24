# SPDX-License-Identifier: GPL-3.0-or-later
# Channel services (negotiator, media selection, support configuration).
# Tests register as channel_services_* for `ctest -R channel_services`.
#
# Cross builds compile the production library; only the test executable and
# registrations are host-side (the test links the AASDK-backed adapter, which
# is native-only per cmake/AasdkAbi.cmake).

add_library(aa_channels STATIC
    src/channels/SupportConfiguration.cpp
    src/channels/Negotiator.cpp
    src/channels/SensorSink.cpp
    src/channels/MicrophoneSink.cpp
    src/channels/InputSink.cpp
    src/channels/AvSink.cpp
    src/channels/MediaSelection.cpp)
target_include_directories(aa_channels PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_channels PUBLIC aa_core)
target_link_libraries(aa_channels PRIVATE aa_input_service)
if(TARGET aa_protocol_adapter)
    target_link_libraries(aa_channels PRIVATE aa_protocol_adapter)
endif()

set(AA_CHANNELS_WARN_TARGETS aa_channels)
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa_channel_services_test
        tests/channels/wifi_projection_test.cpp
        tests/channels/negotiator_test.cpp
        tests/channels/media_wire_test.cpp
        tests/channels/sensor_sink_test.cpp
        tests/channels/microphone_sink_test.cpp
        tests/channels/input_sink_test.cpp
        tests/channels/av_sink_test.cpp
        tests/channels/wire_reject_test.cpp)
    target_link_libraries(aa_channel_services_test PRIVATE
        aa_channels aa_protocol_adapter aap_protobuf aasdk GTest::gtest_main)
    list(APPEND AA_CHANNELS_WARN_TARGETS aa_channel_services_test)
endif()

foreach(target IN LISTS AA_CHANNELS_WARN_TARGETS)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
    if(AA_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
endforeach()

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME channels_wifi_projection COMMAND aa_channel_services_test --gtest_filter=WifiProjectionRouting.*)
    set_tests_properties(channels_wifi_projection PROPERTIES TIMEOUT 60)
    add_test(NAME channel_services_routing COMMAND aa_channel_services_test --gtest_filter=ChannelServices.*)
    add_test(NAME channel_services_media_wire COMMAND aa_channel_services_test --gtest_filter=MediaWire.*)
    add_test(NAME channel_services_wire_reject COMMAND aa_channel_services_test --gtest_filter=ChannelWireReject.*)
    add_test(NAME channel_services_sensors COMMAND aa_channel_services_test --gtest_filter=SensorWire.*)
    add_test(NAME channel_services_microphone COMMAND aa_channel_services_test --gtest_filter=MicrophoneWire.*)
    add_test(NAME channel_services_input COMMAND aa_channel_services_test --gtest_filter=InputWire.*)
    add_test(NAME channel_services_av_sink COMMAND aa_channel_services_test --gtest_filter=AvSinkWire.*)
    set_tests_properties(channel_services_routing channel_services_media_wire channel_services_wire_reject
        PROPERTIES TIMEOUT 60)
endif()

unset(AA_CHANNELS_WARN_TARGETS)
