# SPDX-License-Identifier: GPL-3.0-or-later
# Task 27 approved-phone trust store and pairing policy: a user-owned
# versioned allowlist (0600, atomic replace, XDG state home), the approve-once
# pairing policy wired to the task-21 one-time request-id gate through the
# PhoneDirectory seam, redaction via the task-19 boundary, and fail-closed
# unknown-phone behaviour. Tests register as trust_store_* for
# `ctest -R trust_store`; the bus cases run under their own private bus via
# dbus-run-session.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# cmake/Diagnostics.cmake (aa_diagnostics) and cmake/DbusContract.cmake
# (aa_ipc_control / aa_ipc_dbus), and after the googletest declaration
# (GTest::gtest_main) and Python3.

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Trust store qualification unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_ipc_control)
    message(FATAL_ERROR
        "AA_TRUST_STORE_REQUIRES_AA_IPC_CONTROL: include cmake/DbusContract.cmake before cmake/TrustStore.cmake")
endif()

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
find_program(AA_TRUST_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)

set(AA_TRUST_SOURCES
    src/trust/Identity.cpp
    src/trust/Json.cpp
    src/trust/PairingPolicy.cpp
    src/trust/Store.cpp
    src/trust/StoreFile.cpp
    src/trust/StoreSchema.cpp
    src/trust/StoreWrite.cpp)
set(AA_TRUST_TEST_SOURCES
    tests/trust/store_test.cpp
    tests/trust/store_permissions_test.cpp
    tests/trust/store_migration_test.cpp
    tests/trust/store_schema_test.cpp
    tests/trust/pairing_policy_test.cpp
    tests/trust/pairing_expiry_test.cpp
    tests/trust/redaction_test.cpp
    tests/trust/control_flow_test.cpp)
set(AA_TRUST_BUS_TEST_SOURCES
    tests/trust/bus_flow_test.cpp)
foreach(source IN LISTS AA_TRUST_SOURCES AA_TRUST_TEST_SOURCES AA_TRUST_BUS_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_TRUST_STORE_SOURCE_MISSING: ${source} (trust store source inventory)")
    endif()
endforeach()

add_library(aa_trust STATIC ${AA_TRUST_SOURCES})
set_target_properties(aa_trust PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_trust PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_trust PUBLIC aa_core aa_diagnostics)
target_include_directories(aa_trust PRIVATE "${CMAKE_SOURCE_DIR}/src")

add_executable(aa_trust_store_test ${AA_TRUST_TEST_SOURCES})
target_link_libraries(aa_trust_store_test PRIVATE aa_trust aa_ipc_control GTest::gtest_main)
set_target_properties(aa_trust_store_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(aa_trust_store_test PRIVATE "${CMAKE_SOURCE_DIR}/src")

add_executable(aa_trust_bus_test ${AA_TRUST_BUS_TEST_SOURCES})
target_link_libraries(aa_trust_bus_test PRIVATE aa_trust aa_ipc_dbus GTest::gtest_main)
set_target_properties(aa_trust_bus_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(aa_trust_bus_test PRIVATE "${CMAKE_SOURCE_DIR}/src")

foreach(target aa_trust aa_trust_store_test aa_trust_bus_test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_TRUST_STORE_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_trust_asan_target aa_trust aa_trust_store_test aa_trust_bus_test)
        target_compile_options(${_aa_trust_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_trust_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_trust_asan_target)
endif()

add_test(NAME trust_store_store COMMAND aa_trust_store_test --gtest_filter=TrustStore.*)
add_test(NAME trust_store_permissions COMMAND aa_trust_store_test --gtest_filter=StorePermissions.*)
add_test(NAME trust_store_migration COMMAND aa_trust_store_test --gtest_filter=StoreMigration.*)
add_test(NAME trust_store_schema COMMAND aa_trust_store_test --gtest_filter=StoreSchema.*)
add_test(NAME trust_store_policy COMMAND aa_trust_store_test --gtest_filter=PairingPolicy.*)
add_test(NAME trust_store_expiry COMMAND aa_trust_store_test --gtest_filter=PairingExpiry.*)
add_test(NAME trust_store_redaction COMMAND aa_trust_store_test --gtest_filter=TrustRedaction.*)
add_test(NAME trust_store_control_flow COMMAND aa_trust_store_test --gtest_filter=ControlFlow.*)
add_test(NAME trust_store_bus COMMAND "${AA_TRUST_DBUS_RUN_SESSION}" --
    "$<TARGET_FILE:aa_trust_bus_test>")
set_tests_properties(trust_store_store trust_store_permissions trust_store_migration
    trust_store_schema trust_store_policy trust_store_expiry trust_store_redaction
    trust_store_control_flow trust_store_bus PROPERTIES TIMEOUT 60)

unset(AA_TRUST_SOURCES)
unset(AA_TRUST_TEST_SOURCES)
unset(AA_TRUST_BUS_TEST_SOURCES)
