# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa-wireless-receiver tools/wireless-receiver/main.cpp)
    set_target_properties(aa-wireless-receiver PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(aa-wireless-receiver PRIVATE
        aa_wireless aa_wired aa_protocol_adapter aa_ipc_control aa_ipc_dbus
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
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/wireless-receiver-smoke.cmake" CONTENT [=[
# SPDX-License-Identifier: GPL-3.0-or-later
execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>" --help
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 3)
if(NOT "${result}" STREQUAL "0" OR NOT output MATCHES "usage:")
    message(FATAL_ERROR "--help failed: ${result}: ${error}")
endif()
foreach(arguments IN ITEMS "" "--unknown" "--duration-ms;garbage" "--phone-id;-1"
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
execute_process(COMMAND "$<TARGET_FILE:aa-wireless-receiver>"
    --store-dir "${directory}" --phone-id 1 --duration-ms 100
    --helper-bus-address "unix:path=${directory}/absent-bus"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 5)
file(REMOVE_RECURSE "${directory}")
if(NOT "${result}" STREQUAL "3" OR NOT error MATCHES "system helper connection")
    message(FATAL_ERROR "absent helper failed closed incorrectly: ${result}: ${error}")
endif()
message(STATUS "wireless_receiver_smoke: help=0, invalid arguments=2, absent helper=3 (bounded)")
]=])
    add_test(NAME wireless_receiver_smoke COMMAND "${CMAKE_COMMAND}" -P
        "${CMAKE_CURRENT_BINARY_DIR}/wireless-receiver-smoke.cmake")
    set_tests_properties(wireless_receiver_smoke PROPERTIES TIMEOUT 25)
endif()
