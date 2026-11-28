# SPDX-License-Identifier: GPL-3.0-or-later
# Installed separately from the upstream AASDK install components.
if(AA_ENABLE_DEB_PACKAGE)
    if(CMAKE_CROSSCOMPILING OR NOT CMAKE_BUILD_TYPE STREQUAL "Release"
       OR AA_ENABLE_TEST_HELPER_BUS OR AA_ENABLE_ASAN)
        message(FATAL_ERROR "Debian staging requires the full native Release build with production options")
    endif()

    set(deb_apps aa-wireless-receiver aa-wired-receiver aa-wired-enrol
        aa-dummy-display aa-decode-probe aa-usb-aoa-probe aa-ipc-bench)
    set(deb_libdir lib/android-auto-receiver)
    set_target_properties(aa-wireless-receiver aa-wired-receiver PROPERTIES
        INSTALL_RPATH "$ORIGIN/../${deb_libdir}")
    set_target_properties(aasdk aap_protobuf PROPERTIES INSTALL_RPATH "$ORIGIN")
    install(TARGETS ${deb_apps} RUNTIME DESTINATION bin COMPONENT receiver-deb)
    install(TARGETS aa-system-helper RUNTIME DESTINATION libexec COMPONENT receiver-deb)
    install(TARGETS aasdk aap_protobuf
        LIBRARY DESTINATION "${deb_libdir}" COMPONENT receiver-deb NAMELINK_SKIP)
    install(TARGETS aareceiver-consumer
        ARCHIVE DESTINATION "${deb_libdir}" COMPONENT receiver-deb)
    add_custom_target(receiver-deb-binaries DEPENDS ${deb_apps} aa-system-helper
        aasdk aap_protobuf aareceiver-consumer)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME deb_package_layout
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/packaging/test_deb.py"
                "${CMAKE_C_COMPILER}" "${CMAKE_AR}")
    add_test(NAME deb_package_lintian
        COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/packaging/test_deb.py"
                "${CMAKE_C_COMPILER}" "${CMAKE_AR}" --lintian)
    set_tests_properties(deb_package_layout deb_package_lintian PROPERTIES
        TIMEOUT 90 SKIP_RETURN_CODE 77)
    if(AA_ENABLE_DEB_PACKAGE)
        add_test(NAME deb_package_staged
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/tests/packaging/test_deb.py"
                    "${CMAKE_C_COMPILER}" "${CMAKE_AR}"
                    --cmake "${CMAKE_COMMAND}" --build-dir "${CMAKE_BINARY_DIR}")
        set_tests_properties(deb_package_staged PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    endif()
    add_test(NAME deploy_ssh_harness
        COMMAND "${Python3_EXECUTABLE}" -B -m unittest discover
                -s "${CMAKE_SOURCE_DIR}/tests/deploy" -v)
    set_tests_properties(deploy_ssh_harness PROPERTIES TIMEOUT 240)
endif()
