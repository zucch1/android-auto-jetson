# Explicit project-owned inventory; no vendor source globbing or ABI build.
set(AA_HOST_SMOKE_SOURCES tests/host/smoke.cpp)
foreach(source IN LISTS AA_HOST_SMOKE_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_HOST_SOURCE_MISSING: ${source} (host source inventory)")
    endif()
endforeach()

add_executable(aa_host_smoke ${AA_HOST_SMOKE_SOURCES})
set_target_properties(aa_host_smoke PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_link_libraries(aa_host_smoke PRIVATE GTest::gtest_main)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(aa_host_smoke PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
endif()

option(AA_ENABLE_ASAN "Instrument project-owned host code with AddressSanitizer" OFF)
if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    target_compile_options(aa_host_smoke PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(aa_host_smoke PRIVATE -fsanitize=address)
endif()
add_test(NAME host_cpp20_smoke COMMAND aa_host_smoke)
set_tests_properties(host_cpp20_smoke PROPERTIES TIMEOUT 30)
