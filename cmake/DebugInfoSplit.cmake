# SPDX-License-Identifier: GPL-3.0-or-later
# Host builds with debug info move each project executable's DWARF into a
# sibling <binary>.debug file and leave a .gnu_debuglink behind, so gdb still
# finds the symbols.
#
# Why: the system helper authenticates its D-Bus caller by SHA-256 of
# /proc/PID/exe on every request (Identity.cpp inspect_process, 500 ms
# deadline). The recovery, composed and helper suites run the receiver inside
# the test binary, so the helper re-hashes the test executable 2-3 times per
# eligibility refresh, inside the 950 ms evidence window
# (HandoffEligibility.cpp). A host-dev test binary is ~36 MB, and ~26 MB of
# that is DWARF. Under parallel ctest load those hashes lapsed the deadlines:
# wireless_recovery_R01 failed 20/20 with every core busy, and passed 20/20
# under the same load once the binary was ~9.6 MB.
#
# Only the bytes on disk change. The helper's identity check, its deadlines
# and the evidence window are untouched. --strip-debug keeps .symtab and
# .dynsym, which tools/build/hardening.py reads.
#
# ASan builds keep DWARF inline, so their reports stay symbolized without a
# debuglink lookup. Cross builds are Release and are not split.
option(AA_SPLIT_DEBUG_INFO "Move host executables' DWARF into <binary>.debug" ON)

function(aa_split_debug_info_project_targets)
    if(NOT AA_SPLIT_DEBUG_INFO OR CMAKE_CROSSCOMPILING OR AA_ENABLE_ASAN OR NOT CMAKE_OBJCOPY)
        return()
    endif()
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|RelWithDebInfo)$")
        return()
    endif()
    get_property(_aa_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_aa_target IN LISTS _aa_targets)
        get_target_property(_aa_type ${_aa_target} TYPE)
        if(NOT _aa_type STREQUAL "EXECUTABLE")
            continue()
        endif()
        add_custom_command(TARGET ${_aa_target} POST_BUILD
            COMMAND "${CMAKE_OBJCOPY}" --only-keep-debug "$<TARGET_FILE:${_aa_target}>"
                "$<TARGET_FILE:${_aa_target}>.debug"
            COMMAND "${CMAKE_OBJCOPY}" --strip-debug
                "--add-gnu-debuglink=$<TARGET_FILE_NAME:${_aa_target}>.debug"
                "$<TARGET_FILE:${_aa_target}>"
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:${_aa_target}>"
            VERBATIM)
    endforeach()
endfunction()
