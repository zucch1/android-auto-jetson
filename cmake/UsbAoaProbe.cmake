# SPDX-License-Identifier: GPL-3.0-or-later
# Host-only USB/AOA probe regressions. These never open a real device: the
# boundary modules are exercised with fake transports and sysfs fixture roots.
if(NOT CMAKE_CROSSCOMPILING)
    foreach(case selection handshake libusb sysfs hostenv cycles cli)
        add_test(NAME usb_aoa_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/usb/test_probe.py" ${case})
        set_tests_properties(usb_aoa_${case} PROPERTIES TIMEOUT 60)
    endforeach()
    add_test(NAME usb_aoa_coverage COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/usb/assert_coverage.py" "${CMAKE_BINARY_DIR}")
    set_tests_properties(usb_aoa_coverage PROPERTIES TIMEOUT 60)
endif()
