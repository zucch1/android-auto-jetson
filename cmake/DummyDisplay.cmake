# SPDX-License-Identifier: GPL-3.0-or-later
# Task 24 Qt 6 dummy display consumer: a dummy projection display that talks
# to the receiver through the REAL task-21 D-Bus contract (never bypassing it),
# renders replay streams through the task-22 video channel and the task-23
# consumer helper, normalizes keyboard/D-pad and mouse-to-touch input into the
# task-26 input seam, and exposes a TEST-ONLY local autostart. The task-21
# integration obligations land here too: the live service export (owned
# receiver name/object, continuous dispatch, REAL declared signals) and idle
# owner cleanup via ControlService::refresh_consumer_owner(). Tests register
# as dummy_display_contract_* for `ctest -R dummy_display_contract`.
#
# TEST-ONLY: the autostart gate (aa::display::TestOnlyAutostart) is explicitly
# test-only; no production autostart entry exists or is installed here.
#
# aareceiver-display (the toolkit-free display session logic) is production
# code and compiles in every configuration. The Qt adapter, the dummy display
# executable, the test receiver and the test suites are host tooling: Qt6
# target development metadata is unbound in every sanctioned cross input
# (docs/SYSROOT.md), so the Qt chain stays behind
# if(NOT CMAKE_CROSSCOMPILING) alongside the test registrations. The
# aareceiver-consumer link follows its own task-9 metadata gate.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# cmake/DbusContract.cmake (aa_ipc_dbus), cmake/VideoSocket.cmake (aa_video),
# cmake/InputSocket.cmake (aa_input) and, for the host tooling chain,
# cmake/ConsumerDecode.cmake (aareceiver-consumer) and the googletest
# declaration. Qt includes live only under src/display/qt_adapter/
# (architecture_boundary adapter zone).

if(NOT TARGET aa_core OR NOT TARGET aa_ipc_dbus OR NOT TARGET aa_video OR NOT TARGET aa_input)
    message(FATAL_ERROR
        "AA_DUMMY_DISPLAY_REQUIRES_PRIORS: include cmake/CoreArchitecture.cmake, cmake/DbusContract.cmake, cmake/VideoSocket.cmake and cmake/InputSocket.cmake before cmake/DummyDisplay.cmake")
endif()
if(NOT CMAKE_CROSSCOMPILING AND NOT TARGET aareceiver-consumer)
    message(FATAL_ERROR
        "AA_DUMMY_DISPLAY_REQUIRES_CONSUMER: include cmake/ConsumerDecode.cmake before cmake/DummyDisplay.cmake")
endif()

set(AA_DISPLAY_SOURCES
    src/display/DisplayController.cpp)
set(AA_DISPLAY_QT_SOURCES
    src/display/qt_adapter/ProjectionWidget.cpp)
set(AA_DISPLAY_APP_SOURCES
    tools/dummy-display/main.cpp)
set(AA_DISPLAY_TEST_SOURCES
    tests/display/dummy_display_test.cpp)
set(AA_TEST_RECEIVER_SOURCES
    tools/aa-test-receiver/main.cpp)
foreach(source IN LISTS AA_DISPLAY_SOURCES AA_DISPLAY_QT_SOURCES AA_DISPLAY_APP_SOURCES AA_DISPLAY_TEST_SOURCES AA_TEST_RECEIVER_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_DUMMY_DISPLAY_SOURCE_MISSING: ${source} (dummy display source inventory)")
    endif()
endforeach()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tools/aa-test-receiver/annexb_units.hpp")
    message(FATAL_ERROR "AA_DUMMY_DISPLAY_SOURCE_MISSING: tools/aa-test-receiver/annexb_units.hpp")
endif()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tools/dummy-display/test_control.hpp")
    message(FATAL_ERROR "AA_DUMMY_DISPLAY_SOURCE_MISSING: tools/dummy-display/test_control.hpp")
endif()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/tests/display/downstream_link_check.cpp")
    message(FATAL_ERROR "AA_DUMMY_DISPLAY_SOURCE_MISSING: tests/display/downstream_link_check.cpp")
endif()

add_library(aareceiver-display STATIC ${AA_DISPLAY_SOURCES})
set_target_properties(aareceiver-display PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aareceiver-display PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aareceiver-display PUBLIC aa_ipc_dbus aa_video aa_input aa_core)
if(TARGET aareceiver-consumer)
    target_link_libraries(aareceiver-display PUBLIC aareceiver-consumer)
endif()
# Typed-unavailable decoder factory selection: AA_DISPLAY_HAS_CONSUMER is set
# PRIVATE, based SOLELY on the presence of TARGET aareceiver-consumer (a
# metadata fact of this configuration - never a bypass option). Without the
# consumer the guarded factory rejects channel_decode_unavailable instead of
# referencing helper symbols nothing can resolve.
if(TARGET aareceiver-consumer)
    target_compile_definitions(aareceiver-display PRIVATE AA_DISPLAY_HAS_CONSUMER=1)
else()
    target_compile_definitions(aareceiver-display PRIVATE AA_DISPLAY_HAS_CONSUMER=0)
endif()

if(TARGET aa_sysroot_hash_gate)
    # Root hash-gate convention: this library cannot compile after a
    # post-configure sysroot tamper (present for jetson-aarch64 cross builds).
    add_dependencies(aareceiver-display aa_sysroot_hash_gate)
endif()

set(AA_DISPLAY_WARN_TARGETS aareceiver-display)
if(CMAKE_CROSSCOMPILING)
    # Build-only downstream link check (PR #36 fix round): a consumer-less
    # cross configuration previously linked aareceiver-display fine but left
    # helper_decode_sessions() referencing symbols no archive could resolve -
    # the failure only surfaced at the NEXT project's link. This target links
    # ONLY through aareceiver-display (the controller API and the factory pull
    # the archive objects in) and is built by default in cross configs so the
    # no-overlay build catches the undefined references. It is never executed
    # (AArch64) and deliberately adds NO CTest entry - the cross test
    # inventory stays exact.
    add_executable(aa_display_link_check tests/display/downstream_link_check.cpp)
    target_link_libraries(aa_display_link_check PRIVATE aareceiver-display)
    set_target_properties(aa_display_link_check PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    list(APPEND AA_DISPLAY_WARN_TARGETS aa_display_link_check)
    if(TARGET aa_sysroot_hash_gate)
        add_dependencies(aa_display_link_check aa_sysroot_hash_gate)
    endif()
endif()
if(NOT CMAKE_CROSSCOMPILING)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(AA_QT6 REQUIRED IMPORTED_TARGET
        NO_CMAKE_PATH NO_CMAKE_ENVIRONMENT_PATH
        Qt6Widgets)
    if(NOT AA_DBUS_RUN_SESSION)
        find_program(AA_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)
    endif()

    add_library(aareceiver-display-qt STATIC ${AA_DISPLAY_QT_SOURCES})
    set_target_properties(aareceiver-display-qt PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
    target_include_directories(aareceiver-display-qt PUBLIC "${CMAKE_SOURCE_DIR}/src/display/qt_adapter")
    target_link_libraries(aareceiver-display-qt PUBLIC aareceiver-display PkgConfig::AA_QT6)

    add_executable(aa-dummy-display ${AA_DISPLAY_APP_SOURCES})
    target_link_libraries(aa-dummy-display PRIVATE aareceiver-display-qt)
    set_target_properties(aa-dummy-display PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

    # TEST-ONLY runnable receiver-side integration (task 24 F4): serves the
    # shipped display executable end-to-end outside the gtest harness.
    add_executable(aa-test-receiver ${AA_TEST_RECEIVER_SOURCES})
    target_link_libraries(aa-test-receiver PRIVATE aa_ipc_dbus aa_video aa_input aa_core pthread)
    set_target_properties(aa-test-receiver PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

    add_executable(aa_dummy_display_test ${AA_DISPLAY_TEST_SOURCES})
    target_link_libraries(aa_dummy_display_test PRIVATE
        aareceiver-display-qt GTest::gtest_main pthread)
    set_target_properties(aa_dummy_display_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_definitions(aa_dummy_display_test PRIVATE
        AA_DISPLAY_FIXTURE="${CMAKE_SOURCE_DIR}/tests/fixtures/media/h264_720p30_baseline_90f.h264")

    list(APPEND AA_DISPLAY_WARN_TARGETS
        aareceiver-display-qt aa-dummy-display aa-test-receiver aa_dummy_display_test)
endif()

foreach(target IN LISTS AA_DISPLAY_WARN_TARGETS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_DUMMY_DISPLAY_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_display_asan_target IN LISTS AA_DISPLAY_WARN_TARGETS)
        target_compile_options(${_aa_display_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_display_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_display_asan_target)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    # The suite runs headless (offscreen QPA) on a private bus per test entry.
    add_test(NAME dummy_display_contract_control COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayContractControl.*)
    add_test(NAME dummy_display_contract_render COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayContractRender.*)
    add_test(NAME dummy_display_contract_input COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayContractInput.*)
    add_test(NAME dummy_display_contract_absent COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayContractAbsent.*)
    add_test(NAME dummy_display_contract_idle COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayContractIdle.*)
    add_test(NAME dummy_display_contract_autostart COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayContractAutostart.*)
    # Stop-recovery regressions (PR #36 fix round): the failed-stop decision
    # table against a test-only scripted receiver (real contract name, real
    # read timeouts, recording decode session, real video socket).
    add_test(NAME dummy_display_stop_recovery COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayStopRecovery.*)
    # Registration-lifecycle regressions (PR #36 fix round): failed
    # registrations unwind their real-bus name acquisition, duplicates keep
    # the existing name and unregister releases it (real daemon queueing).
    add_test(NAME dummy_display_registration COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dummy_display_test>" --gtest_filter=DummyDisplayRegistration.*)
    set_tests_properties(dummy_display_contract_control dummy_display_contract_render
        dummy_display_contract_input dummy_display_contract_absent dummy_display_contract_idle
        dummy_display_contract_autostart dummy_display_stop_recovery dummy_display_registration
        PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 180)

    # Executable-level QA (task 24 gate round 2): the SHIPPED aa-dummy-display and
    # the runnable aa-test-receiver run end-to-end on a private bus. replay
    # asserts the real event loop pumps/renders (F1); flow asserts start -> render
    # -> input -> stop without the autostart flag (F2/F4); absent asserts no
    # helper process/request and no AP/network change with no receiver (F5);
    # stalled asserts an UNPACED 401-key burst against a same-UID accept-but-
    # don't-read input endpoint never starves status or F6 stop (gate review F1);
    # stopfail covers the never-replying stop (SIGSTOP, transport_io timeout);
    # stopreject covers the REJECTED stop through the test-only scripted
    # receiver MODE (aa-test-receiver --mode reject-first-stop) - stop_failed
    # plus the typed rejection, no second remote StartProjection, and the
    # accepted retry landing idle with the widget error cleared.
    foreach(case replay flow absent stalled stopfail stopreject)
        add_test(NAME dummy_display_exec_${case} COMMAND "${AA_DBUS_RUN_SESSION}" --
            "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/display/executable_display_test.py"
            --display "$<TARGET_FILE:aa-dummy-display>"
            --receiver "$<TARGET_FILE:aa-test-receiver>"
            --fixture "${CMAKE_SOURCE_DIR}/tests/fixtures/media/h264_720p30_baseline_90f.h264"
            --case ${case})
        set_tests_properties(dummy_display_exec_${case} PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
            TIMEOUT 300)
    endforeach()
endif()

unset(AA_DISPLAY_SOURCES)
unset(AA_DISPLAY_QT_SOURCES)
unset(AA_DISPLAY_APP_SOURCES)
unset(AA_DISPLAY_TEST_SOURCES)
unset(AA_TEST_RECEIVER_SOURCES)
unset(AA_DISPLAY_WARN_TARGETS)
