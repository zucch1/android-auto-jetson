# SPDX-License-Identifier: GPL-3.0-or-later
option(AA_ENABLE_TEST_HELPER_BUS
    "Allow --helper-bus-address in aa-wireless-receiver for host test builds" OFF)
include("${CMAKE_CURRENT_LIST_DIR}/TestHelperBusGate.cmake")
get_property(_aa_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(AA_ENABLE_TEST_HELPER_BUS AND _aa_multi_config)
    message(FATAL_ERROR
        "AA_WIRELESS_TEST_HELPER_BUS_DISALLOWED: --helper-bus-address is not supported with multi-config generators")
endif()
unset(_aa_multi_config)
aa_check_test_helper_bus("${AA_ENABLE_TEST_HELPER_BUS}" "${CMAKE_CROSSCOMPILING}" "${CMAKE_BUILD_TYPE}")

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa-wireless-receiver tools/wireless-receiver/main.cpp)
    add_dependencies(aa-wireless-receiver aa-wired-receiver)
    set_target_properties(aa-wireless-receiver PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_definitions(aa-wireless-receiver PRIVATE
        AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS=$<BOOL:${AA_ENABLE_TEST_HELPER_BUS}>)
    target_link_libraries(aa-wireless-receiver PRIVATE
        aa_audio_worker aa_wireless aa_wired aa_protocol_adapter aa_ipc_control aa_ipc_dbus
        aa_video aa_input aareceiver-audio aa_transport_tls aap_protobuf aasdk pthread)
    if(TARGET PkgConfig::AA_AUDIO_GST)
        target_link_libraries(aa-wireless-receiver PRIVATE PkgConfig::AA_AUDIO_GST)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(aa-wireless-receiver PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
    if(AA_ENABLE_ASAN)
        target_compile_options(aa-wireless-receiver PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(aa-wireless-receiver PRIVATE -fsanitize=address)
    endif()

    add_executable(aa_wireless_receiver_cli_prod_test
        tests/wireless_e2e/receiver_cli_test.cpp tests/wireless_e2e/wired_worker_test.cpp
        tests/wireless_e2e/supervisor_test.cpp)
    target_link_libraries(aa_wireless_receiver_cli_prod_test PRIVATE aa_ipc_control aa_trust)
    add_executable(aa_wireless_receiver_cli_dev_test tests/wireless_e2e/receiver_cli_test.cpp)
    target_compile_definitions(aa_wireless_receiver_cli_prod_test PRIVATE
        AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS=0)
    target_compile_definitions(aa_wireless_receiver_cli_dev_test PRIVATE
        AA_WIRELESS_ALLOW_HELPER_BUS_ADDRESS=1)
    foreach(cli_target aa_wireless_receiver_cli_prod_test aa_wireless_receiver_cli_dev_test)
        set_target_properties(${cli_target} PROPERTIES
            CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
        target_link_libraries(${cli_target} PRIVATE aa_video aa_input aa_core GTest::gtest_main)
        if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
            target_compile_options(${cli_target} PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
        endif()
        if(AA_ENABLE_ASAN)
            target_compile_options(${cli_target} PRIVATE
                -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${cli_target} PRIVATE -fsanitize=address)
        endif()
    endforeach()
    unset(cli_target)
    add_test(NAME wireless_receiver_cli_production COMMAND aa_wireless_receiver_cli_prod_test)
    add_test(NAME wireless_receiver_cli_test_bus COMMAND aa_wireless_receiver_cli_dev_test)
    set_tests_properties(wireless_receiver_cli_production wireless_receiver_cli_test_bus
        PROPERTIES TIMEOUT 15)

    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wireless-receiver-smoke.cmake" CONTENT [=[
# SPDX-License-Identifier: GPL-3.0-or-later
execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>" --help
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
if(NOT "${result}" STREQUAL "0" OR NOT output MATCHES "usage:")
    message(FATAL_ERROR "--help failed: ${result}: ${error}")
endif()
if(AA_ALLOW_HELPER_BUS STREQUAL "1")
    if(NOT output MATCHES "--helper-bus-address")
        message(FATAL_ERROR "test-enabled --help omitted --helper-bus-address")
    endif()
else()
    if(output MATCHES "--helper-bus-address")
        message(FATAL_ERROR "production --help exposed --helper-bus-address")
    endif()
endif()
foreach(arguments IN ITEMS "" "--unknown" "--duration-ms;garbage" "--phone-id;-1"
        "--store-dir;/tmp" "--store-dir;/tmp;--pairing-only;--phone-id;1"
        "--store-dir;/tmp;--phone-id;1;--duration-ms;1tail")
    execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>" ${arguments}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
    if(NOT "${result}" STREQUAL "2")
        message(FATAL_ERROR "invalid arguments returned ${result}: ${error}")
    endif()
endforeach()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(directory "${CMAKE_CURRENT_BINARY_DIR}/wireless-smoke-${nonce}")
file(MAKE_DIRECTORY "${directory}")
if(AA_ALLOW_HELPER_BUS STREQUAL "1")
    execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>"
        --store-dir "${directory}" --phone-id 1 --duration-ms 100
        --helper-bus-address "unix:path=${directory}/absent-bus"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT "${result}" STREQUAL "3" OR NOT error MATCHES "system helper connection")
        file(REMOVE_RECURSE "${directory}")
        message(FATAL_ERROR "absent helper failed closed incorrectly: ${result}: ${error}")
    endif()
    execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>"
        --store-dir "${directory}" --pairing-only --duration-ms 100
        --helper-bus-address "unix:path=${directory}/absent-bus"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    file(REMOVE_RECURSE "${directory}")
    if(NOT "${result}" STREQUAL "3" OR NOT error MATCHES "system helper connection")
        message(FATAL_ERROR "pairing-only did not reach helper without an approved id: ${result}: ${error}")
    endif()
    message(STATUS "wireless_receiver_smoke: help=0, invalid arguments=2, normal/pairing-only absent helper=3 (bounded)")
else()
    execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>"
        --store-dir "${directory}" --phone-id 1 --duration-ms 100
        --helper-bus-address "unix:path=${directory}/absent-bus"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT "${result}" STREQUAL "2" OR NOT error MATCHES "--helper-bus-address is only supported in test builds")
        file(REMOVE_RECURSE "${directory}")
        message(FATAL_ERROR "production build did not reject --helper-bus-address: ${result}: ${error}")
    endif()
    execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>"
        --store-dir "${directory}" --pairing-only --duration-ms 100
        --helper-bus-address "unix:path=${directory}/absent-bus"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT "${result}" STREQUAL "2" OR NOT error MATCHES "--helper-bus-address is only supported in test builds")
        file(REMOVE_RECURSE "${directory}")
        message(FATAL_ERROR "production build pairing-only did not reject --helper-bus-address: ${result}: ${error}")
    endif()
    execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>"
        --store-dir "${directory}" --phone-id 1 "--helper-bus-address=unix:path=${directory}/absent-bus"
        --duration-ms 100
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    if(NOT "${result}" STREQUAL "2" OR NOT error MATCHES "--helper-bus-address is only supported in test builds")
        file(REMOVE_RECURSE "${directory}")
        message(FATAL_ERROR "production build did not reject --helper-bus-address=: ${result}: ${error}")
    endif()
    execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>"
        --store-dir "${directory}" --phone-id 1 --helper-bus-address
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
    file(REMOVE_RECURSE "${directory}")
    if(NOT "${result}" STREQUAL "2" OR NOT error MATCHES "--helper-bus-address is only supported in test builds")
        message(FATAL_ERROR "production build did not reject bare --helper-bus-address: ${result}: ${error}")
    endif()
    message(STATUS "wireless_receiver_smoke: help=0, invalid arguments=2, production --helper-bus-address rejected=2 (bounded)")
endif()
]=])
    set(_aa_allow_helper_bus 0)
    if(AA_ENABLE_TEST_HELPER_BUS)
        set(_aa_allow_helper_bus 1)
        add_test(NAME wireless_receiver_helper_startup COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/helper/private_bus.py" "${CMAKE_BINARY_DIR}" receiver-startup)
        set_tests_properties(wireless_receiver_helper_startup PROPERTIES TIMEOUT 20)
    endif()
    add_test(NAME wireless_receiver_smoke COMMAND "${CMAKE_COMMAND}"
        "-DAA_ALLOW_HELPER_BUS=${_aa_allow_helper_bus}"
        -P "${CMAKE_CURRENT_BINARY_DIR}/wireless-receiver-smoke.cmake")
    unset(_aa_allow_helper_bus)
    add_test(NAME wireless_receiver_helper_bus_gate COMMAND "${CMAKE_COMMAND}"
        "-DAA_GATE=${CMAKE_CURRENT_LIST_DIR}/TestHelperBusGate.cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/cmake/test_helper_bus_gate.cmake")
    set_tests_properties(wireless_receiver_smoke PROPERTIES TIMEOUT 25)
endif()
