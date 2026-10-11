# SPDX-License-Identifier: GPL-3.0-or-later
# Task 26 capability-gated input and sensor services: receiver-side input
# normalization over the task-24 input socket seam plus the no-default sensor
# service. Tests register as input_sensors_* for `ctest -R input_sensors`.
#
# Structural isolation (gate r2): aa_input_service is the PRODUCTION library
# and physically contains no fixture enablement or synthetic emission. The
# test-only simulated sensor fixture lives in aa_input_sensor_fixtures, which
# is linked ONLY into the test binary (regression: input_sensors_isolation).
# Memory-checked coverage (gate r1): input_sensors_memory runs the suite under
# valgrind/memcheck with --error-exitcode=99.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# cmake/InputSocket.cmake (aa_input - the transport seam the tests consume)
# and the googletest declaration.

if(CMAKE_CROSSCOMPILING)
    message(STATUS "Input/sensor service qualification unavailable in cross-smoke mode")
    return()
endif()
if(NOT TARGET aa_input)
    message(FATAL_ERROR
        "AA_INPUT_SENSORS_REQUIRES_AA_INPUT: include cmake/InputSocket.cmake before cmake/InputSensors.cmake")
endif()
find_program(AA_VALGRIND_EXECUTABLE NAMES valgrind)
if(NOT AA_VALGRIND_EXECUTABLE)
    message(FATAL_ERROR
        "AA_INPUT_SENSORS_MEMCHECK_REQUIRES_VALGRIND: the memory-checked input/sensor coverage is mandatory (gate r1)")
endif()

set(AA_INPUT_SERVICE_SOURCES
    src/input/Capability.cpp
    src/input/InputService.cpp
    src/input/SensorService.cpp)
set(AA_INPUT_SENSOR_FIXTURE_SOURCES
    tests/input_sensors/SimulatedSensorFixture.cpp)
set(AA_INPUT_SERVICE_TEST_SOURCES
    tests/input_sensors/capability_test.cpp
    tests/input_sensors/input_service_test.cpp
    tests/input_sensors/sensor_service_test.cpp)
foreach(source IN LISTS AA_INPUT_SERVICE_SOURCES AA_INPUT_SENSOR_FIXTURE_SOURCES AA_INPUT_SERVICE_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_INPUT_SERVICE_SOURCE_MISSING: ${source} (input service source inventory)")
    endif()
endforeach()

add_library(aa_input_service STATIC ${AA_INPUT_SERVICE_SOURCES})
set_target_properties(aa_input_service PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_input_service PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_input_service PUBLIC aa_core)

# TEST-ONLY support library: never linked into a production target.
add_library(aa_input_sensor_fixtures STATIC ${AA_INPUT_SENSOR_FIXTURE_SOURCES})
set_target_properties(aa_input_sensor_fixtures PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_input_sensor_fixtures PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_input_sensor_fixtures PUBLIC aa_core)

add_executable(aa_input_sensors_test ${AA_INPUT_SERVICE_TEST_SOURCES})
target_link_libraries(aa_input_sensors_test PRIVATE
    aa_input_service aa_input_sensor_fixtures aa_input aa_core GTest::gtest_main)
set_target_properties(aa_input_sensors_test PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)

foreach(target aa_input_service aa_input_sensor_fixtures aa_input_sensors_test)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_INPUT_SERVICE_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_input_service_asan_target aa_input_service aa_input_sensor_fixtures aa_input_sensors_test)
        target_compile_options(${_aa_input_service_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_input_service_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_input_service_asan_target)
endif()

add_test(NAME input_sensors_capability COMMAND aa_input_sensors_test --gtest_filter=InputSensorsCapability.*)
add_test(NAME input_sensors_input COMMAND aa_input_sensors_test --gtest_filter=InputSensorsInput.*)
add_test(NAME input_sensors_sensors COMMAND aa_input_sensors_test --gtest_filter=InputSensorsSensor.*)
add_test(NAME input_sensors_memory COMMAND "${AA_VALGRIND_EXECUTABLE}"
    --tool=memcheck --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite
    --track-origins=yes $<TARGET_FILE:aa_input_sensors_test>)
add_test(NAME input_sensors_isolation COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/input_sensors/production_isolation_test.py"
    "${CMAKE_CXX_COMPILER}"
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/tests/input_sensors"
    "$<TARGET_FILE:aa_input_service>"
    "$<TARGET_FILE:aa_input_sensor_fixtures>")
set_tests_properties(input_sensors_capability input_sensors_input input_sensors_sensors
    PROPERTIES TIMEOUT 120)
set_tests_properties(input_sensors_memory PROPERTIES TIMEOUT 600)
set_tests_properties(input_sensors_isolation PROPERTIES TIMEOUT 120)

unset(AA_INPUT_SERVICE_SOURCES)
unset(AA_INPUT_SENSOR_FIXTURE_SOURCES)
unset(AA_INPUT_SERVICE_TEST_SOURCES)
