# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa_tls_hardening_test tests/tls/hardening_test.cpp)
    target_link_libraries(aa_tls_hardening_test PRIVATE aa_transport_tls GTest::gtest_main)
    target_link_options(aa_tls_hardening_test PRIVATE -Wl,--wrap=OPENSSL_cleanse -Wl,--wrap=SSL_read)
    set_target_properties(aa_tls_hardening_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_options(aa_tls_hardening_test PRIVATE -Wall -Wextra -Wpedantic -Werror)
    add_test(NAME tls_hardening COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/transport/tls_run.py"
        "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_tls_hardening_test>")
    set_tests_properties(tls_hardening PROPERTIES TIMEOUT 60)
endif()
