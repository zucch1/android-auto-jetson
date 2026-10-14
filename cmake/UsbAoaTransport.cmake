# SPDX-License-Identifier: GPL-3.0-or-later
# Task 28 wired USB/AOA transport: the real AOA bulk ByteEndpoint adapter under
# src/transport/usb_adapter/ (the only place the libusb ABI surface appears).
# Linkage decision: the adapter dlopen()s libusb-1.0.so.0 at run time and
# declares the ABI entry points it uses, so no libusb header or import library
# is required on host or in the frozen cross sysroot (which carries runtime
# libusb only). The library and the aa-usb-aoa-probe tool therefore compile
# UNCONDITIONALLY (host + jetson-aarch64 cross); tests are host-only.
#
# Test registrations are named usb_aoa_* so the plan acceptance regex
# (`ctest -R usb_aoa`) covers them alongside the existing Python probe
# regressions (cmake/UsbAoaProbe.cmake).

set(AA_USB_AOA_SOURCES
    src/transport/usb_adapter/LibusbAbi.cpp
    src/transport/usb_adapter/UsbAoaSwitch.cpp
    src/transport/usb_adapter/UsbAoaEndpoint.cpp
    src/transport/usb_adapter/LibusbWorld.cpp)
set(AA_USB_AOA_PROBE_SOURCES
    tools/usb-aoa-probe/main.cpp)
set(AA_USB_AOA_TEST_SOURCES
    tests/transport/usb_aoa_switch_test.cpp
    tests/transport/usb_aoa_endpoint_test.cpp
    tests/transport/usb_aoa_hotplug_test.cpp
    tests/transport/usb_aoa_identity_test.cpp
    tests/transport/usb_aoa_probe_test.cpp
    tests/transport/usb_aoa_world_test.cpp)
foreach(source IN LISTS AA_USB_AOA_SOURCES AA_USB_AOA_PROBE_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_USB_AOA_SOURCE_MISSING: ${source}")
    endif()
endforeach()

add_library(aa_transport_usb STATIC ${AA_USB_AOA_SOURCES})
set_target_properties(aa_transport_usb PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_transport_usb PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_transport_usb PUBLIC aa_transport aa_trust aa_core ${CMAKE_DL_LIBS})

add_executable(aa-usb-aoa-probe ${AA_USB_AOA_PROBE_SOURCES})
set_target_properties(aa-usb-aoa-probe PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_link_libraries(aa-usb-aoa-probe PRIVATE aa_transport_usb)

set(AA_USB_AOA_WARN_TARGETS aa_transport_usb aa-usb-aoa-probe)
if(NOT CMAKE_CROSSCOMPILING)
    foreach(source IN LISTS AA_USB_AOA_TEST_SOURCES)
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
            message(FATAL_ERROR "AA_USB_AOA_TEST_SOURCE_MISSING: ${source}")
        endif()
    endforeach()

    # Fake libusb runtime (pre-merge fix round): built as libusb-1.0.so.0 so the
    # adapter's real dlopen path loads it via LD_LIBRARY_PATH. Host-only test
    # harness; never installed, never linked by production code. It MUST live in
    # its own subdirectory: libaasdk.so.0 carries a trailing-empty RUNPATH entry
    # (CWD), so a fake named libusb-1.0.so.0 in the build root would shadow the
    # real libusb for every test that links AASDK.
    add_library(aa_fake_libusb SHARED tests/transport/fake_libusb.cpp)
    set_target_properties(aa_fake_libusb PROPERTIES
        PREFIX "" OUTPUT_NAME "libusb-1.0.so.0" SUFFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fake-libusb"
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(aa_fake_libusb PUBLIC "${CMAKE_SOURCE_DIR}/include")
    list(APPEND AA_USB_AOA_WARN_TARGETS aa_fake_libusb)

    add_executable(aa_usb_aoa_test ${AA_USB_AOA_TEST_SOURCES})
    set_target_properties(aa_usb_aoa_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(aa_usb_aoa_test PRIVATE aa_transport_usb aa_session GTest::gtest_main)
    list(APPEND AA_USB_AOA_WARN_TARGETS aa_usb_aoa_test)

    add_test(NAME usb_aoa_switch COMMAND aa_usb_aoa_test --gtest_filter=UsbAoaSwitch.*)
    add_test(NAME usb_aoa_endpoint COMMAND aa_usb_aoa_test --gtest_filter=UsbAoaEndpoint.*)
    add_test(NAME usb_aoa_hotplug COMMAND aa_usb_aoa_test --gtest_filter=UsbAoaHotplug.*)
    add_test(NAME usb_aoa_identity COMMAND aa_usb_aoa_test --gtest_filter=UsbAoaIdentity.*)
    add_test(NAME usb_aoa_probe COMMAND aa_usb_aoa_test --gtest_filter=UsbAoaProbe.*)
    add_test(NAME usb_aoa_world COMMAND "${CMAKE_COMMAND}" -E env
        "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:aa_fake_libusb>:$ENV{LD_LIBRARY_PATH}"
        "$<TARGET_FILE:aa_usb_aoa_test>" --gtest_filter=UsbAoaWorld.*)
    foreach(case absent_json hostile_port timeout_parse)
        add_test(NAME usb_aoa_probe_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/usb/test_probe_binary.py" ${case}
            "$<TARGET_FILE:aa-usb-aoa-probe>")
    endforeach()
    foreach(case setup_exit watchdog)
        add_test(NAME usb_aoa_probe_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/usb/test_probe_binary.py" ${case}
            "$<TARGET_FILE:aa-usb-aoa-probe>" "$<TARGET_FILE:aa_fake_libusb>")
    endforeach()
    set_tests_properties(usb_aoa_switch usb_aoa_endpoint usb_aoa_hotplug usb_aoa_identity
        usb_aoa_probe usb_aoa_world usb_aoa_probe_absent_json usb_aoa_probe_hostile_port
        usb_aoa_probe_timeout_parse usb_aoa_probe_setup_exit usb_aoa_probe_watchdog
        PROPERTIES TIMEOUT 60)
endif()

foreach(target IN LISTS AA_USB_AOA_WARN_TARGETS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_USB_AOA_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_asan_target IN LISTS AA_USB_AOA_WARN_TARGETS)
        target_compile_options(${_aa_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_asan_target)
endif()

if(TARGET aa_sysroot_hash_gate)
    add_dependencies(aa_transport_usb aa_sysroot_hash_gate)
    add_dependencies(aa-usb-aoa-probe aa_sysroot_hash_gate)
endif()

unset(AA_USB_AOA_SOURCES)
unset(AA_USB_AOA_PROBE_SOURCES)
unset(AA_USB_AOA_TEST_SOURCES)
unset(AA_USB_AOA_WARN_TARGETS)
