# Task-2 delivery wiring: provenance gate only.
# Delivery-derived file (written for this task-2 delivery snapshot); it is not
# original upstream history content. The stage2/4 shared foundation CMake block
# (dependency staging, GoogleTest, host smoke, C/CXX) is intentionally excluded.
cmake_minimum_required(VERSION 3.20)
project(android_auto_provenance LANGUAGES NONE)

find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
enable_testing()

add_test(NAME provenance_check
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tools/provenance/check.py"
            --root "${CMAKE_SOURCE_DIR}")

foreach(case baseline overlay content header extra generated license gpl inventory listing symlink missing)
    add_test(NAME provenance_isolation_${case}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/provenance/scenarios.py"
                "${CMAKE_SOURCE_DIR}" "${case}")
    set_tests_properties(provenance_isolation_${case} PROPERTIES
        ENVIRONMENT "PYTHON=${Python3_EXECUTABLE}" TIMEOUT 60)
endforeach()
