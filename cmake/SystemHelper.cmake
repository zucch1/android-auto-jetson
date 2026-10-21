# SPDX-License-Identifier: GPL-3.0-or-later
if(CMAKE_CROSSCOMPILING)
    message(STATUS "System helper qualification unavailable in cross-smoke mode")
    return()
endif()
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
add_library(aa_system_helper STATIC
    src/helper/Identity.cpp src/helper/SystemHelper.cpp src/helper/BusService.cpp
    src/helper/BusClient.cpp src/helper/crypto_adapter/Crypto.cpp)
target_include_directories(aa_system_helper PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_system_helper PUBLIC aa_ipc_dbus aa_trust OpenSSL::Crypto)
add_executable(aa-system-helper src/helper/main.cpp)
target_link_libraries(aa-system-helper PRIVATE aa_system_helper)
foreach(target aa_system_helper aa-system-helper)
    set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endforeach()
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa_system_helper_test tests/helper/core_test.cpp tests/helper/identity_test.cpp
        tests/helper/confirmation_test.cpp)
    target_link_libraries(aa_system_helper_test PRIVATE aa_system_helper GTest::gtest_main)
    set_target_properties(aa_system_helper_test PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
    add_test(NAME system_helper_unit COMMAND aa_system_helper_test)
    set_tests_properties(system_helper_unit PROPERTIES TIMEOUT 60)
    add_executable(aa_system_helper_fixture tests/helper/fixture.cpp)
    target_link_libraries(aa_system_helper_fixture PRIVATE aa_system_helper)
    add_executable(aa_system_helper_bus_test tests/helper/bus_test.cpp)
    target_link_libraries(aa_system_helper_bus_test PRIVATE aa_system_helper GTest::gtest_main)
    foreach(target aa_system_helper_fixture aa_system_helper_bus_test)
        set_target_properties(${target} PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES)
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endforeach()
    add_test(NAME system_helper_private_bus COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/helper/private_bus.py" "${CMAKE_BINARY_DIR}")
    set_tests_properties(system_helper_private_bus PROPERTIES TIMEOUT 90)
    add_test(NAME system_helper_delivery_policy COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/helper/policy_bus.py" "${CMAKE_BINARY_DIR}")
    set_tests_properties(system_helper_delivery_policy PROPERTIES TIMEOUT 30)
    add_test(NAME system_helper_cross_smoke_configuration COMMAND "${CMAKE_COMMAND}"
        -DCMAKE_CROSSCOMPILING=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE
        -P "${CMAKE_SOURCE_DIR}/cmake/SystemHelper.cmake")
endif()
