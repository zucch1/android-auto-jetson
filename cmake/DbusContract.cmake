# SPDX-License-Identifier: GPL-3.0-or-later
# Task 21 versioned session D-Bus control contract: the introspection XML in
# docs/dbus is the contract authority; tools/ipc/dbus_contract.py validates it
# against the checked schema (malformed XML, unknown major versions and
# high-rate payloads fail with named diagnostics at build time) and embeds it
# as the runtime introspection. aa_ipc_control holds the authorization seam and
# dispatch semantics (pure project code); aa_ipc_dbus holds the real D-Bus
# peer-credential lookup over a private-bus-tested minimal bus client. Tests
# register as dbus_contract_* for `ctest -R dbus_contract`; the bus cases run
# each on their own private bus via dbus-run-session.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# the googletest declaration (GTest::gtest_main) and Python3. The parent adds
# `include(cmake/DbusContract.cmake)` in its own root edit.

if(NOT TARGET aa_core)
    message(FATAL_ERROR
        "AA_DBUS_CONTRACT_REQUIRES_AA_CORE: include cmake/CoreArchitecture.cmake before cmake/DbusContract.cmake")
endif()

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)

set(AA_DBUS_CONTRACT_XML "${CMAKE_SOURCE_DIR}/docs/dbus/org.custom.AndroidAutoReceiver1.xml")
set(AA_DBUS_CONTRACT_TOOL "${CMAKE_SOURCE_DIR}/tools/ipc/dbus_contract.py")
set(AA_DBUS_CONTRACT_SCHEMA "${CMAKE_SOURCE_DIR}/tools/ipc/dbus_contract_schema.py")
set(AA_DBUS_GENERATED_DIR "${CMAKE_BINARY_DIR}/generated")
set(AA_DBUS_GENERATED_HEADER "${AA_DBUS_GENERATED_DIR}/aa/ipc/dbus_contract.generated.hpp")

set(AA_IPC_CONTROL_SOURCES
    src/ipc/Contract.cpp
    src/ipc/PairingGate.cpp
    src/ipc/ControlService.cpp)
set(AA_IPC_DBUS_SOURCES
    src/ipc/dbus_adapter/BusWire.cpp
    src/ipc/dbus_adapter/BusParse.cpp
    src/ipc/dbus_adapter/BusMessage.cpp
    src/ipc/dbus_adapter/BusConnection.cpp
    src/ipc/dbus_adapter/BusCalls.cpp
    src/ipc/dbus_adapter/BusPeerLookup.cpp
    src/ipc/dbus_adapter/ControlDispatcher.cpp
    src/ipc/dbus_adapter/PropertiesDispatch.cpp)
set(AA_DBUS_TEST_SOURCES
    tests/dbus/control_service_test.cpp
    tests/dbus/pairing_gate_test.cpp
    tests/dbus/introspection_test.cpp
    tests/dbus/dispatch_test.cpp
    tests/dbus/bus_lookup_test.cpp
    tests/dbus/bus_boundary_test.cpp
    tests/dbus/bus_spoof_test.cpp)
foreach(source IN LISTS AA_IPC_CONTROL_SOURCES AA_IPC_DBUS_SOURCES AA_DBUS_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_DBUS_CONTRACT_SOURCE_MISSING: ${source} (dbus contract source inventory)")
    endif()
endforeach()
if(NOT EXISTS "${AA_DBUS_CONTRACT_XML}")
    message(FATAL_ERROR "AA_DBUS_CONTRACT_XML_MISSING: ${AA_DBUS_CONTRACT_XML}")
endif()

# Schema validation + codegen: a malformed or drifted contract XML fails the
# build here with a named DBUS_CONTRACT_* diagnostic (never silently accepted).
add_custom_command(
    OUTPUT "${AA_DBUS_GENERATED_HEADER}"
    COMMAND "${Python3_EXECUTABLE}" -B "${AA_DBUS_CONTRACT_TOOL}" generate
            --xml "${AA_DBUS_CONTRACT_XML}" --out "${AA_DBUS_GENERATED_HEADER}"
    DEPENDS "${AA_DBUS_CONTRACT_XML}" "${AA_DBUS_CONTRACT_TOOL}" "${AA_DBUS_CONTRACT_SCHEMA}"
    COMMENT "aa ipc: validating and embedding the D-Bus control contract"
    VERBATIM)
add_custom_target(aa_dbus_contract_codegen DEPENDS "${AA_DBUS_GENERATED_HEADER}")

add_library(aa_ipc_control STATIC ${AA_IPC_CONTROL_SOURCES})
set_target_properties(aa_ipc_control PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_ipc_control PUBLIC
    "${CMAKE_SOURCE_DIR}/include" "${AA_DBUS_GENERATED_DIR}")
target_link_libraries(aa_ipc_control PUBLIC aa_core)
add_dependencies(aa_ipc_control aa_dbus_contract_codegen)

add_library(aa_ipc_dbus STATIC ${AA_IPC_DBUS_SOURCES})
set_target_properties(aa_ipc_dbus PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_ipc_dbus PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_ipc_dbus PUBLIC aa_ipc_control)

if(TARGET aa_sysroot_hash_gate)
    # Root hash-gate convention: these libraries cannot compile after a
    # post-configure sysroot tamper (present for jetson-aarch64 cross builds).
    add_dependencies(aa_ipc_control aa_sysroot_hash_gate)
    add_dependencies(aa_ipc_dbus aa_sysroot_hash_gate)
endif()

set(AA_DBUS_WARN_TARGETS aa_ipc_control aa_ipc_dbus)
if(NOT CMAKE_CROSSCOMPILING)
    find_program(AA_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)
    add_executable(aa_dbus_contract_test ${AA_DBUS_TEST_SOURCES})
    target_link_libraries(aa_dbus_contract_test PRIVATE aa_ipc_dbus GTest::gtest_main)
    set_target_properties(aa_dbus_contract_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_definitions(aa_dbus_contract_test PRIVATE
        AA_DBUS_CONTRACT_XML="${AA_DBUS_CONTRACT_XML}")
    list(APPEND AA_DBUS_WARN_TARGETS aa_dbus_contract_test)
endif()

foreach(target IN LISTS AA_DBUS_WARN_TARGETS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_DBUS_CONTRACT_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_dbus_asan_target IN LISTS AA_DBUS_WARN_TARGETS)
        target_compile_options(${_aa_dbus_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_dbus_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_dbus_asan_target)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME dbus_contract_control COMMAND aa_dbus_contract_test --gtest_filter=ControlService.*)
    add_test(NAME dbus_contract_access COMMAND aa_dbus_contract_test --gtest_filter=AccessControl.*)
    add_test(NAME dbus_contract_pairing COMMAND aa_dbus_contract_test
        --gtest_filter=PairingGate.*:PairingFlow.*)
    add_test(NAME dbus_contract_introspection COMMAND aa_dbus_contract_test --gtest_filter=Introspection.*)
    add_test(NAME dbus_contract_dispatch COMMAND aa_dbus_contract_test --gtest_filter=CallBoundary.*)
    add_test(NAME dbus_contract_bus COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dbus_contract_test>" --gtest_filter=BusLookup.*:BusFlow.*)
    add_test(NAME dbus_contract_boundary COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dbus_contract_test>" --gtest_filter=BusBoundary.*)
    add_test(NAME dbus_contract_spoof COMMAND "${AA_DBUS_RUN_SESSION}" --
        "$<TARGET_FILE:aa_dbus_contract_test>" --gtest_filter=BusSpoof.*)
    set_tests_properties(dbus_contract_control dbus_contract_access dbus_contract_pairing
        dbus_contract_introspection dbus_contract_dispatch dbus_contract_bus dbus_contract_boundary
        dbus_contract_spoof PROPERTIES TIMEOUT 60)
endif()

set(AA_DBUS_SCHEMA_CASES
    real-check real-generate
    neg-malformed-xml neg-unsupported-major neg-overflow-major neg-high-rate-payload
    neg-missing-method neg-bad-signature neg-semver-mismatch neg-unexpected-method
    neg-duplicate-method neg-bogus-direction neg-masking-duplicate
    neg-multi-type-method neg-multi-type-signal
    neg-raw-delimiter-injection embedding-special-bytes embedding-crlf)
if(NOT CMAKE_CROSSCOMPILING)
    foreach(case IN LISTS AA_DBUS_SCHEMA_CASES)
        string(REPLACE "-" "_" test_suffix "${case}")
        add_test(NAME dbus_contract_schema_${test_suffix}
            COMMAND "${Python3_EXECUTABLE}" -B
                    "${CMAKE_SOURCE_DIR}/tests/dbus/contract_scenarios.py"
                    "${CMAKE_SOURCE_DIR}" "${case}" "${CMAKE_CXX_COMPILER}")
        set_tests_properties(dbus_contract_schema_${test_suffix} PROPERTIES TIMEOUT 60)
    endforeach()
    unset(case)
    unset(test_suffix)

    add_test(NAME dbus_contract_coverage
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/dbus/assert_dbus_coverage.py"
                "${CMAKE_BINARY_DIR}")
    set_tests_properties(dbus_contract_coverage PROPERTIES TIMEOUT 60)
endif()

unset(AA_IPC_CONTROL_SOURCES)
unset(AA_IPC_DBUS_SOURCES)
unset(AA_DBUS_TEST_SOURCES)
unset(AA_DBUS_SCHEMA_CASES)
unset(AA_DBUS_WARN_TARGETS)
