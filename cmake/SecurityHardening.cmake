# SPDX-License-Identifier: GPL-3.0-or-later
if(CMAKE_CROSSCOMPILING)
    return()
endif()

# Keep the focused profile selectable by either name or label. Reuse the native
# test binaries and fixtures so the security checks exercise the same code paths
# as the component suites and run without provisioned services or hardware.
add_test(NAME security_hardening_framing COMMAND aa_transport_frame_test
    --gtest_filter=FuzzSmoke.*:Malformed.*)
add_test(NAME security_hardening_protobuf COMMAND aa_protocol_adapter_test
    --gtest_filter=MessageReject.*)
add_test(NAME security_hardening_ipc_authority COMMAND "${AA_DBUS_RUN_SESSION}" --
    "$<TARGET_FILE:aa_dbus_contract_test>"
    --gtest_filter=AccessControl.*:ControlService.*:CallBoundary.*:BusBoundary.*:BusSocketAuth.*)
add_test(NAME security_hardening_video_peer COMMAND aa_video_socket_test
    --gtest_filter=VideoPeer.*)
add_test(NAME security_hardening_input_peer COMMAND aa_input_socket_test
    --gtest_filter=InputPeer.*)
add_test(NAME security_hardening_helper_authority COMMAND aa_system_helper_test
    --gtest_filter=HelperCore.*:HelperIdentity.*)
add_test(NAME security_hardening_unknown_device COMMAND aa_usb_aoa_test
    --gtest_filter=UsbAoaIdentity.*)
add_test(NAME security_hardening_trust_store COMMAND aa_trust_store_test
    --gtest_filter=StorePermissions.*:StoreSchema.*:TrustStore.UnknownPhoneFailsClosed)
add_test(NAME security_hardening_diagnostics
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/diagnostics/test_json.py"
        "$<TARGET_FILE:aa_diagnostics_json_probe>")

add_test(NAME security_hardening_transport_tls
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/transport/tls_run.py"
        "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_transport_tls_test>")
add_test(NAME security_hardening_authorized_tls
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/tls/handshake.py"
        "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_tls_handshake>" approved)

foreach(case insecure symlink hardlink malformed)
    add_test(NAME security_hardening_credential_${case}
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/tls/credentials.py"
            "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_credential_probe>" "${case}")
endforeach()

add_test(NAME security_hardening_secret_scan
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/tls/scan.py" "${CMAKE_SOURCE_DIR}")
add_test(NAME security_hardening_secret_scan_negative
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/tls/scan.py" "${CMAKE_SOURCE_DIR}" --negative)

get_property(_aa_security_tests DIRECTORY PROPERTY TESTS)
list(FILTER _aa_security_tests INCLUDE REGEX "^security_hardening_")
set_tests_properties(${_aa_security_tests} PROPERTIES LABELS security_hardening TIMEOUT 60)
unset(_aa_security_tests)
# Fail closed if a GoogleTest filter silently matches zero tests after a rename.
set_tests_properties(
    security_hardening_framing security_hardening_protobuf security_hardening_ipc_authority
    security_hardening_video_peer security_hardening_input_peer security_hardening_helper_authority
    security_hardening_unknown_device security_hardening_trust_store
    PROPERTIES FAIL_REGULAR_EXPRESSION "\\[==========\\] 0 tests from 0 test (suites|cases) ran\\.")
# Preserve the isolation used by the source suites for their timed IPC work.
set_tests_properties(security_hardening_ipc_authority security_hardening_helper_authority
    PROPERTIES RUN_SERIAL TRUE)

add_custom_target(security_hardening_tests DEPENDS
    aa_transport_frame_test aa_transport_tls_test aa_protocol_adapter_test aa_dbus_contract_test
    aa_video_socket_test aa_input_socket_test aa_system_helper_test
    aa_usb_aoa_test aa_trust_store_test aa_diagnostics_json_probe aa_tls_handshake aa_credential_probe)
