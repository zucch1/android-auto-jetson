# Native qualification only: the frozen cross sysroot has no AASDK development inputs.
if(CMAKE_CROSSCOMPILING)
    message(STATUS "AASDK ABI qualification unavailable in cross-smoke mode")
    return()
endif()

find_package(Protobuf REQUIRED)
execute_process(COMMAND "${Protobuf_PROTOC_EXECUTABLE}" --version
    OUTPUT_VARIABLE aa_protoc_version OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE aa_protoc_result)
if(NOT aa_protoc_result EQUAL 0 OR NOT aa_protoc_version STREQUAL "libprotoc ${Protobuf_VERSION}")
    message(FATAL_ERROR "AA_PROTOBUF_VERSION_MISMATCH: protoc and native runtime must match")
endif()
set(SKIP_BUILD_PROTOBUF ON CACHE BOOL "Use native verified protobuf" FORCE)
set(SKIP_BUILD_ABSL ON CACHE BOOL "No unpinned Abseil fetch" FORCE)
set(AASDK_TEST OFF CACHE BOOL "Project ABI suite qualifies production sources" FORCE)
set(AASDK_VERSION_OVERRIDE "0.0.0-task7" CACHE STRING "Pinned source qualification" FORCE)
add_subdirectory("${AA_DEPENDENCY_STAGE}/aasdk" "${CMAKE_BINARY_DIR}/aasdk" EXCLUDE_FROM_ALL)
# Both upstream directories declare 17; override the actual targets, not a parent default.
set_target_properties(aasdk aap_protobuf PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_include_directories(aasdk PUBLIC "$<BUILD_INTERFACE:${AA_DEPENDENCY_STAGE}/aasdk/include>")
target_include_directories(aasdk PUBLIC "$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>")
target_link_libraries(aasdk PRIVATE aa_tls)
target_link_options(aasdk PRIVATE -Wl,--no-undefined)
target_link_options(aap_protobuf PRIVATE -Wl,--no-undefined)

add_executable(aa_aasdk_abi tests/aasdk/consumer.cpp)
set_target_properties(aa_aasdk_abi PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_link_libraries(aa_aasdk_abi PRIVATE aasdk)
target_link_options(aa_aasdk_abi PRIVATE -Wl,--no-undefined)
add_test(NAME aasdk_abi_consumer COMMAND aa_aasdk_abi)
add_test(NAME aasdk_abi_symbols COMMAND "${CMAKE_COMMAND}"
    "-DNM=${CMAKE_NM}" "-DCONSUMER=$<TARGET_FILE:aa_aasdk_abi>"
    "-DSDK=$<TARGET_FILE:aasdk>" "-DPROTO=$<TARGET_FILE:aap_protobuf>"
    -P "${CMAKE_SOURCE_DIR}/tests/aasdk/symbols.cmake")
add_test(NAME aasdk_abi_standard_negative COMMAND "${CMAKE_COMMAND}"
    "-DCXX=${CMAKE_CXX_COMPILER}" "-DROOT=${CMAKE_SOURCE_DIR}"
    "-DINCLUDE=${AA_DEPENDENCY_STAGE}/aasdk/include"
    "-DGENERATED=${CMAKE_BINARY_DIR}/aasdk/protobuf"
    "-DSDK=$<TARGET_FILE:aasdk>" "-DPROTO=$<TARGET_FILE:aap_protobuf>"
    "-DBUILD=${CMAKE_BINARY_DIR}" -P "${CMAKE_SOURCE_DIR}/tests/aasdk/standard-negative.cmake")
add_test(NAME aasdk_abi_compile_flags COMMAND "${CMAKE_COMMAND}"
    "-DBUILD=${CMAKE_BINARY_DIR}" "-DSTAGE=${AA_DEPENDENCY_STAGE}"
    -P "${CMAKE_SOURCE_DIR}/tests/aasdk/compile-flags.cmake")
set_tests_properties(aasdk_abi_consumer aasdk_abi_symbols aasdk_abi_standard_negative aasdk_abi_compile_flags
    PROPERTIES TIMEOUT 60)
foreach(case tls tls-tamper)
    add_test(NAME tls_posture_stage_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/deps/staging.py" "${CMAKE_SOURCE_DIR}" "${case}" "${AA_GOOGLETEST_ARCHIVE}")
endforeach()
foreach(case tls-patch tls-lock)
    add_test(NAME tls_posture_manifest_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/deps/scenarios.py" "${CMAKE_SOURCE_DIR}" "${case}" "${AA_GOOGLETEST_ARCHIVE}")
endforeach()
add_test(NAME tls_posture_effective_stage COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/tls/effective.py" "${AA_DEPENDENCY_STAGE}" "${CMAKE_BINARY_DIR}")
