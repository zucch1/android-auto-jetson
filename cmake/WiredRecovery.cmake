# SPDX-License-Identifier: GPL-3.0-or-later
# Task 31 wired reconnect and recovery matrix: the executable recovery matrix
# over the task-30 integrator (aa::app::WiredSession). It adds NO production
# source - it exercises the already-qualified wiring layer (link loss/replug,
# receiver restart, consumer restart, malformed input, unknown phone, phone
# lock/unlock + phone-side restart) and asserts the task-19 diagnostics report
# retry counts and failure outcomes. Tests register as wired_recovery_* so the
# plan acceptance regex (`ctest -R wired_recovery`) covers them.
#
# Integration contract: include AFTER cmake/WiredE2e.cmake (aa_wired) and
# cmake/AudioPipeline.cmake (the task-25 audio router the harness header
# optionally binds). Host-only: the recovery executable and its registrations
# are not created in cross builds.

if(NOT TARGET aa_wired)
    message(FATAL_ERROR
        "AA_WIRED_RECOVERY_REQUIRES_AA_WIRED: include cmake/WiredE2e.cmake before cmake/WiredRecovery.cmake")
endif()

set(AA_WIRED_RECOVERY_TEST_SOURCES
    tests/wired_e2e/wired_recovery_test.cpp)
foreach(source IN LISTS AA_WIRED_RECOVERY_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR
            "AA_WIRED_RECOVERY_SOURCE_MISSING: ${source} (wired recovery source inventory)")
    endif()
endforeach()

if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa_wired_recovery_test ${AA_WIRED_RECOVERY_TEST_SOURCES})
    set_target_properties(aa_wired_recovery_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(aa_wired_recovery_test PRIVATE
        aa_wired aa_protocol_adapter aap_protobuf aasdk
        aa_input aa_input_service aareceiver-audio pthread GTest::gtest_main)
    if(TARGET PkgConfig::AA_AUDIO_GST)
        target_link_libraries(aa_wired_recovery_test PRIVATE PkgConfig::AA_AUDIO_GST)
    endif()
    target_compile_definitions(aa_wired_recovery_test PRIVATE AA_WIRED_HAS_AUDIO_ROUTER=1)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(aa_wired_recovery_test PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
    if(AA_ENABLE_ASAN)
        if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
            message(FATAL_ERROR "AA_WIRED_RECOVERY_ASAN_UNSUPPORTED: requires GNU or Clang")
        endif()
        target_compile_options(aa_wired_recovery_test PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(aa_wired_recovery_test PRIVATE -fsanitize=address)
    endif()

    add_test(NAME wired_recovery_unplug_replug
        COMMAND aa_wired_recovery_test --gtest_filter=WiredRecoveryUnplugReplug.*)
    add_test(NAME wired_recovery_receiver_restart
        COMMAND aa_wired_recovery_test --gtest_filter=WiredRecoveryReceiverRestart.*)
    add_test(NAME wired_recovery_consumer_restart
        COMMAND aa_wired_recovery_test --gtest_filter=WiredRecoveryConsumerRestart.*)
    add_test(NAME wired_recovery_malformed
        COMMAND aa_wired_recovery_test --gtest_filter=WiredRecoveryMalformed.*)
    add_test(NAME wired_recovery_unknown_phone
        COMMAND aa_wired_recovery_test --gtest_filter=WiredRecoveryUnknownPhone.*)
    add_test(NAME wired_recovery_lock_unlock
        COMMAND aa_wired_recovery_test --gtest_filter=WiredRecoveryPhoneLockUnlock.*)
    add_test(NAME wired_recovery_real_clock
        COMMAND aa_wired_recovery_test --gtest_filter=WiredRecoveryRealClock.*)
    add_test(NAME wired_recovery_qa_gates COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/wired_e2e/test_recovery_qa_gates.py"
        "${CMAKE_SOURCE_DIR}/tools/wired-qa/run-wired-recovery-qa.sh")
    set_tests_properties(
        wired_recovery_unplug_replug wired_recovery_receiver_restart
        wired_recovery_consumer_restart wired_recovery_malformed
        wired_recovery_unknown_phone wired_recovery_lock_unlock
        wired_recovery_real_clock wired_recovery_qa_gates
        PROPERTIES TIMEOUT 120)
    set_tests_properties(wired_recovery_qa_gates PROPERTIES TIMEOUT 300)
endif()

unset(AA_WIRED_RECOVERY_TEST_SOURCES)
