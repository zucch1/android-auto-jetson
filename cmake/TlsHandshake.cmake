# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT TARGET aasdk)
    return()
endif()
add_executable(aa_tls_handshake tests/tls/handshake.cpp)
target_link_libraries(aa_tls_handshake PRIVATE aasdk aa_tls)
set_target_properties(aa_tls_handshake PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(aa_tls_handshake PRIVATE -Wall -Wextra -Wpedantic -Werror)
foreach(case approved unknown absent verified-unknown verified-selfsigned compat-selfsigned revoked inactive deinit)
    add_test(NAME tls_posture_handshake_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/tls/handshake.py" "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_tls_handshake>" "${case}")
    set_tests_properties(tls_posture_handshake_${case} PROPERTIES TIMEOUT 30)
endforeach()
