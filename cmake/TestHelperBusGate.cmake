# SPDX-License-Identifier: GPL-3.0-or-later
# --helper-bus-address points aa-wireless-receiver at a private helper bus.
# It is only allowed in native Debug test builds (#168). Kept in its own file
# so tests/cmake/test_helper_bus_gate.cmake can exercise it with cmake -P.
function(aa_check_test_helper_bus enabled crosscompiling build_type)
    if(enabled AND (crosscompiling OR NOT build_type STREQUAL "Debug"))
        message(FATAL_ERROR
            "AA_WIRELESS_TEST_HELPER_BUS_DISALLOWED: --helper-bus-address is only permitted in native Debug test builds")
    endif()
endfunction()
