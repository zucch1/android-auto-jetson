# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
# Exercises cmake/TestHelperBusGate.cmake (#168): the private helper bus flag
# must be rejected for every non-Debug or cross build and allowed only for a
# native Debug build. Each case runs in a child cmake -P so a FATAL_ERROR is
# observable as a nonzero exit.
if(NOT AA_GATE)
    message(FATAL_ERROR "AA_GATE not set")
endif()
set(probe "${CMAKE_CURRENT_BINARY_DIR}/helper-bus-gate-probe.cmake")
file(WRITE "${probe}" "cmake_minimum_required(VERSION 3.20)\ninclude(\"\${AA_GATE}\")\naa_check_test_helper_bus(\"\${E}\" \"\${X}\" \"\${T}\")\n")
# enabled;crosscompiling;build_type;expected(0 ok, 1 rejected)
set(cases
    "ON|OFF|Debug|0" "ON|OFF|Release|1" "ON|OFF|RelWithDebInfo|1" "ON|OFF|MinSizeRel|1"
    "ON|OFF||1" "ON|OFF|debug|1" "ON|ON|Debug|1" "ON|ON|Release|1"
    "OFF|OFF|Release|0" "OFF|ON|Release|0" "OFF|OFF|Debug|0")
foreach(case IN LISTS cases)
    string(REPLACE "|" ";" f "${case}")
    list(GET f 0 e)
    list(GET f 1 x)
    list(GET f 2 t)
    list(GET f 3 want)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DAA_GATE=${AA_GATE}" "-DE=${e}" "-DX=${x}" "-DT=${t}" -P "${probe}"
        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_VARIABLE err)
    if(want STREQUAL "0" AND NOT rc STREQUAL "0")
        message(FATAL_ERROR "gate rejected allowed case ${case}: ${err}")
    elseif(want STREQUAL "1" AND (rc STREQUAL "0" OR NOT err MATCHES "AA_WIRELESS_TEST_HELPER_BUS_DISALLOWED"))
        message(FATAL_ERROR "gate accepted disallowed case ${case}")
    endif()
endforeach()
file(REMOVE "${probe}")
message(STATUS "helper bus gate: all cases behaved")
