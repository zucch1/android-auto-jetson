# SPDX-License-Identifier: GPL-3.0-or-later
# Host-only USB policy fixtures: rule matching, install/rollback state machine
# (fake root) and recorded udevadm-test parsing. No host udev state is touched;
# the real `udevadm test` evidence runs on the target (task-29 receipt).
if(NOT CMAKE_CROSSCOMPILING)
    foreach(case rules rollback udevadm)
        add_test(NAME usb_policy_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/usb/test_usb_policy.py" ${case})
        set_tests_properties(usb_policy_${case} PROPERTIES TIMEOUT 60)
    endforeach()
endif()
