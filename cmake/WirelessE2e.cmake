# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CMAKE_CROSSCOMPILING)
    add_library(aa_wireless STATIC src/app/HandoffEligibility.cpp src/app/HandoffOperations.cpp src/app/WirelessLink.cpp)
    target_link_libraries(aa_wireless PUBLIC aa_wired aa_system_helper aa_transport_tls)
    target_compile_features(aa_wireless PUBLIC cxx_std_20)
    target_compile_options(aa_wireless PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_executable(aa_wireless_test tests/transport/wireless_eligibility_test.cpp)
    target_link_libraries(aa_wireless_test PRIVATE aa_wireless GTest::gtest_main)
    target_compile_options(aa_wireless_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME wireless_e2e_eligibility COMMAND aa_wireless_test --gtest_filter=WirelessEligibility.*)
    set_tests_properties(wireless_e2e_eligibility PROPERTIES TIMEOUT 30)
    add_executable(aa_wireless_production_test tests/wireless_e2e/production_test.cpp tests/wireless_e2e/control_test.cpp)
    target_link_libraries(aa_wireless_production_test PRIVATE aa_wireless GTest::gtest_main)
    target_compile_options(aa_wireless_production_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME wireless_e2e_production COMMAND aa_wireless_production_test)
    set_tests_properties(wireless_e2e_production PROPERTIES TIMEOUT 30)
    add_executable(aa_wireless_video_profiles_test tests/wireless_e2e/video_profiles_test.cpp)
    target_link_libraries(aa_wireless_video_profiles_test PRIVATE aa_wireless GTest::gtest_main)
    target_compile_options(aa_wireless_video_profiles_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME wireless_e2e_video_profiles COMMAND aa_wireless_video_profiles_test
        --gtest_filter=WirelessVideoProfiles.*)
    set_tests_properties(wireless_e2e_video_profiles PROPERTIES TIMEOUT 30)
    add_executable(aa_wireless_attested_test tests/wireless_e2e/attested_test.cpp)
    target_link_libraries(aa_wireless_attested_test PRIVATE aa_wireless GTest::gtest_main)
    target_compile_options(aa_wireless_attested_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME wireless_e2e_attested COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/transport/tls_run.py" "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_wireless_attested_test>")
    set_tests_properties(wireless_e2e_attested PROPERTIES TIMEOUT 60)
    add_executable(aa_wireless_control_io_test tests/wireless_e2e/control_io_test.cpp tests/wireless_e2e/control_active_test.cpp tests/trust/syscall_faults.cpp)
    target_link_options(aa_wireless_control_io_test PRIVATE -Wl,--wrap=fsync -Wl,--wrap=renameat
        -Wl,--wrap=_ZN2aa9transport14encode_messageERKNS0_7MessageENS0_10EncryptionEPNS0_13PayloadCipherE)
    target_link_libraries(aa_wireless_control_io_test PRIVATE aa_wireless aa_video GTest::gtest_main)
    target_compile_options(aa_wireless_control_io_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_test(NAME wireless_e2e_control_io COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/transport/tls_run.py" "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_wireless_control_io_test>")
    set_tests_properties(wireless_e2e_control_io PROPERTIES TIMEOUT 60)
    add_executable(aa_wireless_composed_test tests/wireless_e2e/composed_test.cpp tests/trust/syscall_faults.cpp)
    target_link_libraries(aa_wireless_composed_test PRIVATE aa_wireless GTest::gtest_main PkgConfig::AA_GIO)
    target_compile_definitions(aa_wireless_composed_test PRIVATE AA_HELPER_BUILD="${CMAKE_BINARY_DIR}" AA_HELPER_SOURCE="${CMAKE_SOURCE_DIR}")
    target_link_options(aa_wireless_composed_test PRIVATE -Wl,--wrap=fsync -Wl,--wrap=renameat)
    target_compile_options(aa_wireless_composed_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_dependencies(aa_wireless_composed_test aa-system-helper)
    add_test(NAME wireless_e2e_composed_coordinator COMMAND aa_wireless_composed_test
        --gtest_filter=WirelessComposed.CoordinatorUsesRealCompletedAuthorityAndPinsApOnApprovedReconnect --gtest_repeat=5)
    set_tests_properties(wireless_e2e_composed_coordinator PROPERTIES TIMEOUT 30 RESOURCE_LOCK aa_wireless_composed)
    add_test(NAME wireless_e2e_composed_consumer_loss COMMAND aa_wireless_composed_test
        --gtest_filter=WirelessComposed.*Bootstrap* --gtest_repeat=2)
    set_tests_properties(wireless_e2e_composed_consumer_loss PROPERTIES TIMEOUT 30 RESOURCE_LOCK aa_wireless_composed)
    add_test(NAME wireless_e2e_composed_late_denial COMMAND aa_wireless_composed_test
        --gtest_filter=WirelessComposed.ExpiredHandoffDeniedLateLeavesNoLeaseForTheFreshAttempt:WirelessComposed.RetiredObservationInFlightLeavesNoLeaseForTheFreshAttempt:WirelessComposed.FailedInitialObservationAfterBindingLeavesNoLeaseForTheFreshAttempt)
    set_tests_properties(wireless_e2e_composed_late_denial PROPERTIES TIMEOUT 30 RESOURCE_LOCK aa_wireless_composed)
    add_executable(aa_wireless_composed_tls_test tests/wireless_e2e/composed_tls_test.cpp tests/trust/syscall_faults.cpp)
    target_link_libraries(aa_wireless_composed_tls_test PRIVATE aa_wireless GTest::gtest_main PkgConfig::AA_GIO)
    target_compile_definitions(aa_wireless_composed_tls_test PRIVATE AA_HELPER_BUILD="${CMAKE_BINARY_DIR}" AA_HELPER_SOURCE="${CMAKE_SOURCE_DIR}")
    # Real helper authentication hashes this fixture's entire ELF on every call.
    # Keep DWARF available without making debug data dominate freshness budgets.
    target_link_options(aa_wireless_composed_tls_test PRIVATE -Wl,--wrap=fsync -Wl,--wrap=renameat -Wl,--compress-debug-sections=zlib)
    target_compile_options(aa_wireless_composed_tls_test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    add_dependencies(aa_wireless_composed_tls_test aa-system-helper)
    add_test(NAME wireless_e2e_composed_tls COMMAND "${CMAKE_COMMAND}" -E env "GTEST_FILTER=WirelessComposedTls.*" "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/transport/tls_run.py" "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_wireless_composed_tls_test>"
        )
    set_tests_properties(wireless_e2e_composed_tls PROPERTIES TIMEOUT 60 RESOURCE_LOCK aa_wireless_composed)
    add_test(NAME wireless_e2e_composed_admission COMMAND aa_wireless_composed_test --gtest_filter=Chain/WirelessD6Admission.*)
    add_test(NAME wireless_e2e_composed_bootstrap COMMAND aa_wireless_composed_test
        --gtest_filter=Wire/WirelessD6Bootstrap.*:CompletedLifecycle.WirelessD6*)
    set_tests_properties(wireless_e2e_composed_admission wireless_e2e_composed_bootstrap
        PROPERTIES TIMEOUT 30 RESOURCE_LOCK aa_wireless_composed)
    find_program(AA_DBUS_RUN_SESSION NAMES dbus-run-session REQUIRED)
    add_executable(aa_wireless_audio_worker_test
        tests/wireless_e2e/audio_worker_test.cpp
        tests/wireless_e2e/audio_runtime_test.cpp)
    target_link_libraries(aa_wireless_audio_worker_test PRIVATE
        aa_audio_worker aa_wireless aa_video aa_ipc_dbus aa_input GTest::gtest_main)
    if(TARGET PkgConfig::AA_AUDIO_GST)
        target_link_libraries(aa_wireless_audio_worker_test PRIVATE PkgConfig::AA_AUDIO_GST)
    endif()
    target_compile_options(aa_wireless_audio_worker_test PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    if(AA_ENABLE_ASAN)
        target_compile_options(aa_wireless_audio_worker_test PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(aa_wireless_audio_worker_test PRIVATE -fsanitize=address)
    endif()
    add_test(NAME wireless_e2e_audio_worker
        COMMAND "${AA_DBUS_RUN_SESSION}" -- "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/transport/tls_run.py"
                "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_wireless_audio_worker_test>")
    set_tests_properties(wireless_e2e_audio_worker PROPERTIES
        TIMEOUT 120 ENVIRONMENT "AA_TLS_RUN_TIMEOUT=110")
    add_test(NAME wireless_e2e_audio_worker_telemetry
        COMMAND "${AA_DBUS_RUN_SESSION}" -- "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/wireless_e2e/test_audio_worker_telemetry.py"
                "${CMAKE_SOURCE_DIR}" "$<TARGET_FILE:aa_wireless_audio_worker_test>")
    set_tests_properties(wireless_e2e_audio_worker_telemetry PROPERTIES TIMEOUT 90)
    add_test(NAME wireless_e2e_playout_av_alignment
        COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_SOURCE_DIR}/tests/wireless_e2e/test_playout_av_alignment.py"
                "${CMAKE_SOURCE_DIR}")
    set_tests_properties(wireless_e2e_playout_av_alignment PROPERTIES TIMEOUT 60)
    include("${CMAKE_SOURCE_DIR}/cmake/WirelessMatrix.cmake")
endif()
