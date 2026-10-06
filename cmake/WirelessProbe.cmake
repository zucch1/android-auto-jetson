# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CMAKE_CROSSCOMPILING)
    foreach(case policy discovery active cli window bluez)
        add_test(NAME wireless_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/wireless/test_probe.py" ${case})
        set_tests_properties(wireless_${case} PROPERTIES TIMEOUT 30)
    endforeach()
endif()
