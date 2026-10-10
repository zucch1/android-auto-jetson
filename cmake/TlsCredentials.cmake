# SPDX-License-Identifier: GPL-3.0-or-later
if(CMAKE_CROSSCOMPILING)
    message(STATUS "TLS posture qualification unavailable in cross-smoke mode")
    return()
endif()
find_package(OpenSSL REQUIRED)
add_library(aa_tls STATIC src/tls/Credentials.cpp src/tls/Policy.cpp)
set_target_properties(aa_tls PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_tls PUBLIC "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(aa_tls PUBLIC OpenSSL::SSL OpenSSL::Crypto)
target_compile_options(aa_tls PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(aa_credential_probe tests/tls/credential_probe.cpp)
target_link_libraries(aa_credential_probe PRIVATE aa_tls)
set_target_properties(aa_credential_probe PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(aa_credential_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
foreach(case valid unset empty unreadable insecure malformed mismatch symlink hardlink trailing)
    add_test(NAME tls_posture_credential_${case}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/tls/credentials.py"
                "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_credential_probe>" "${case}")
endforeach()
add_test(NAME tls_posture_reference COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/tls/reference.py" "${CMAKE_SOURCE_DIR}"
    "$<TARGET_FILE:aa_credential_probe>" "$<TARGET_FILE:aa_tls_policy>")
add_test(NAME tls_posture_key_scan COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/tls/scan.py" "${CMAKE_SOURCE_DIR}")
add_test(NAME tls_posture_key_scan_negative COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/tls/scan.py" "${CMAKE_SOURCE_DIR}" --negative)
add_test(NAME tls_posture_key_scan_reencodings COMMAND "${Python3_EXECUTABLE}" -B
    "${CMAKE_SOURCE_DIR}/tests/tls/scan.py" "${CMAKE_SOURCE_DIR}" --reencodings)
foreach(case nonexistent-root regular-file-root unreadable-descendant)
    add_test(NAME tls_posture_key_scan_input_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/tls/scan.py" "${CMAKE_SOURCE_DIR}" --invalid-root ${case})
endforeach()
foreach(case repository-root source-subdirectory external-into-tree external-tmp)
    add_test(NAME tls_posture_credential_generation_${case} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/tls/synthetic_credential.py" "${CMAKE_SOURCE_DIR}" ${case})
endforeach()
add_executable(aa_tls_policy tests/tls/policy.cpp)
target_link_libraries(aa_tls_policy PRIVATE aa_tls)
set_target_properties(aa_tls_policy PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
target_compile_options(aa_tls_policy PRIVATE -Wall -Wextra -Wpedantic -Werror)
foreach(case approved unknown absent throwing verified)
    add_test(NAME tls_posture_policy_${case} COMMAND aa_tls_policy "${case}")
endforeach()
