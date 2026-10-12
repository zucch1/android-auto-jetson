# SPDX-License-Identifier: GPL-3.0-or-later
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Channel services qualification unavailable in cross-smoke mode")
    return()
endif()

add_library(aa_channels STATIC
    src/channels/SupportConfiguration.cpp
    src/channels/Negotiator.cpp
    src/channels/MediaSelection.cpp)
target_include_directories(aa_channels PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_channels PUBLIC aa_core)
add_executable(aa_channel_services_test
    tests/channels/negotiator_test.cpp
    tests/channels/media_wire_test.cpp
    tests/channels/wire_reject_test.cpp)
target_link_libraries(aa_channel_services_test PRIVATE
    aa_channels aa_protocol_adapter aap_protobuf aasdk GTest::gtest_main)
foreach(target aa_channels aa_channel_services_test)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
    if(AA_ENABLE_ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${target} PRIVATE -fsanitize=address)
    endif()
endforeach()
add_test(NAME channel_services_routing COMMAND aa_channel_services_test --gtest_filter=ChannelServices.*)
add_test(NAME channel_services_media_wire COMMAND aa_channel_services_test --gtest_filter=MediaWire.*)
add_test(NAME channel_services_wire_reject COMMAND aa_channel_services_test --gtest_filter=ChannelWireReject.*)
set_tests_properties(channel_services_routing channel_services_media_wire channel_services_wire_reject
    PROPERTIES TIMEOUT 60)
