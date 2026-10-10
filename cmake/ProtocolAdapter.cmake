# SPDX-License-Identifier: GPL-3.0-or-later
# Task 15 protocol adapter: a project-owned protocol schema
# (include/aa/protocol/Messages.hpp) bound to the pinned AASDK protobuf code by
# a private adapter under src/protocol/aasdk_adapter/. The adapter LINKS the
# existing aap_protobuf target generated from the staged dependency
# (AASDK 9bf6adf933665dee26532201719fac14a047ccf1); it never regenerates or
# overlays alternate schemas. OAA reference schemas are rejected by the codegen
# guard (tools/protocol/codegen_guard.py) over real generated output.

# Native qualification only: the frozen cross sysroot has no AASDK development
# inputs (same boundary as cmake/AasdkAbi.cmake).
if(CMAKE_CROSSCOMPILING)
    message(STATUS "Protocol adapter qualification unavailable in cross-smoke mode")
    return()
endif()

if(NOT TARGET aap_protobuf)
    message(FATAL_ERROR
        "AA_PROTOCOL_ADAPTER_REQUIRES_AAP_PROTOBUF: bind the existing pinned "
        "aap_protobuf target from the staged AASDK dependency; never regenerate schemas")
endif()

add_library(aa_protocol_adapter STATIC
    src/protocol/aasdk_adapter/ControlAdapter.cpp
    src/protocol/aasdk_adapter/ServiceAdapter.cpp
    src/protocol/aasdk_adapter/MediaConfiguration.cpp
    src/protocol/aasdk_adapter/ChannelAdapter.cpp)
set_target_properties(aa_protocol_adapter PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_protocol_adapter PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_include_directories(aa_protocol_adapter PRIVATE
    "${CMAKE_BINARY_DIR}/aasdk/protobuf"
    "${AA_DEPENDENCY_STAGE}/aasdk/include"
    ${Protobuf_INCLUDE_DIRS})
target_link_libraries(aa_protocol_adapter PUBLIC aa_core PRIVATE aap_protobuf)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_protocol_adapter PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

set(AA_PROTOCOL_TEST_SOURCES
    tests/protocol/messages_roundtrip_test.cpp
    tests/protocol/messages_reject_test.cpp
    tests/protocol/channel_map_test.cpp)
foreach(source IN LISTS AA_PROTOCOL_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_PROTOCOL_TEST_SOURCE_MISSING: ${source}")
    endif()
endforeach()

add_executable(aa_protocol_adapter_test ${AA_PROTOCOL_TEST_SOURCES})
set_target_properties(aa_protocol_adapter_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_link_libraries(aa_protocol_adapter_test PRIVATE
    aa_protocol_adapter aap_protobuf aasdk GTest::gtest_main)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_protocol_adapter_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()
if(AA_ENABLE_ASAN)
    foreach(_aa_asan_target aa_protocol_adapter aa_protocol_adapter_test)
        target_compile_options(${_aa_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_asan_target)
endif()

add_test(NAME protocol_adapter_messages COMMAND aa_protocol_adapter_test
    --gtest_filter=ControlMessages.*)
add_test(NAME protocol_adapter_reject COMMAND aa_protocol_adapter_test
    --gtest_filter=MessageReject.*)
add_test(NAME protocol_adapter_channel_map COMMAND aa_protocol_adapter_test
    --gtest_filter=ChannelMapping.*)
set_tests_properties(protocol_adapter_messages protocol_adapter_reject protocol_adapter_channel_map
    PROPERTIES TIMEOUT 60)

set(AA_PROTOCOL_CODEGEN_CASES
    real-generated real-protos empty-root
    neg-oaa-include neg-oaa-source neg-oaa-namespace neg-unexpected-root
    neg-oaa-import neg-oaa-package)
foreach(case IN LISTS AA_PROTOCOL_CODEGEN_CASES)
    string(REPLACE "-" "_" test_suffix "${case}")
    add_test(NAME protocol_adapter_codegen_${test_suffix}
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/protocol/codegen_scenarios.py"
                "${CMAKE_SOURCE_DIR}" "${case}"
                "${CMAKE_BINARY_DIR}/aasdk/protobuf"
                "${AA_DEPENDENCY_STAGE}/aasdk/protobuf")
    set_tests_properties(protocol_adapter_codegen_${test_suffix} PROPERTIES TIMEOUT 60)
endforeach()
unset(case)
unset(test_suffix)
unset(AA_PROTOCOL_CODEGEN_CASES)
unset(AA_PROTOCOL_TEST_SOURCES)
