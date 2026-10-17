# SPDX-License-Identifier: GPL-3.0-or-later
# Task 30 wired end-to-end projection integrator: the composition layer that
# wires the qualified modules (USB/AOA transport -> session Lifecycle ->
# frame codec -> channel negotiator -> video producer / audio router / input
# service) into one wired projection path, with task-19 diagnostics along it.
# Tests register as wired_e2e_* for `ctest -R wired_e2e`.
#
# The library compiles in every configuration (project headers only, no external
# include - no *_adapter/ zone needed); only the test executable and its
# registrations are host-side.
#
# Integration contract: include AFTER cmake/CoreArchitecture.cmake (aa_core),
# cmake/ProtocolAdapter.cmake (aa_protocol_adapter), cmake/Channels.cmake,
# cmake/Diagnostics.cmake, cmake/Transport.cmake, cmake/Session.cmake,
# cmake/Replay.cmake, cmake/VideoSocket.cmake (aa_video),
# cmake/InputSocket.cmake (aa_input), cmake/InputSensors.cmake
# (aa_input_service) and cmake/AudioPipeline.cmake (aareceiver-audio, native).

if(NOT TARGET aa_core)
    message(FATAL_ERROR
        "AA_WIRED_E2E_REQUIRES_AA_CORE: include cmake/CoreArchitecture.cmake before cmake/WiredE2e.cmake")
endif()
if(NOT TARGET aa_input_service)
    message(FATAL_ERROR
        "AA_WIRED_E2E_REQUIRES_AA_INPUT_SERVICE: include cmake/InputSensors.cmake before cmake/WiredE2e.cmake")
endif()

set(AA_WIRED_SOURCES
    src/app/WiredPhoneControl.cpp
    src/app/WiredSession.cpp
    src/app/WiredLink.cpp)
set(AA_WIRED_TEST_SOURCES
    tests/trust/syscall_faults.cpp
    tests/wired_e2e/wired_e2e_happy_test.cpp
    tests/wired_e2e/wired_e2e_revoke_test.cpp
    tests/wired_e2e/wired_e2e_failures_test.cpp
    tests/wired_e2e/receiver_test.cpp
    tests/wired_e2e/av_sink_test.cpp
    tests/wired_e2e/microphone_test.cpp
    tests/wired_e2e/wired_deadline_test.cpp
    tests/wired_e2e/wired_e2e_trust_link_test.cpp
    tests/wired_e2e/wired_e2e_input_wire_test.cpp
    tests/wired_e2e/wired_e2e_auth_test.cpp
    tests/wired_e2e/wired_e2e_control_test.cpp
    tests/wired_e2e/wired_e2e_recovery_test.cpp)
foreach(source IN LISTS AA_WIRED_SOURCES AA_WIRED_TEST_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
        message(FATAL_ERROR "AA_WIRED_SOURCE_MISSING: ${source} (wired e2e source inventory)")
    endif()
endforeach()

add_library(aa_wired STATIC ${AA_WIRED_SOURCES})
set_target_properties(aa_wired PROPERTIES
    CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO
    POSITION_INDEPENDENT_CODE ON)
target_include_directories(aa_wired PUBLIC "${CMAKE_SOURCE_DIR}/include")
# The AASDK-backed protocol codec (protocol::encode/decode) is native-only; the
# archive compiles without it in cross configurations (nothing links aa_wired
# into a cross executable).
set(AA_WIRED_LINK
    aa_core aa_session aa_channels aa_transport aa_transport_usb aa_trust aa_diagnostics
    aa_input aa_input_service)
if(TARGET aa_protocol_adapter)
    list(APPEND AA_WIRED_LINK aa_protocol_adapter)
endif()
target_link_libraries(aa_wired PUBLIC ${AA_WIRED_LINK})

if(TARGET aa_sysroot_hash_gate)
    # Root hash-gate convention: this library cannot compile after a
    # post-configure sysroot tamper (present for jetson-aarch64 cross builds).
    add_dependencies(aa_wired aa_sysroot_hash_gate)
endif()

set(AA_WIRED_WARN_TARGETS aa_wired)
if(NOT CMAKE_CROSSCOMPILING)
    # The real task-25 audio router is a mandatory host component (the audio
    # pipeline is required natively); the wired test wires it, never a stub.
    if(NOT TARGET aareceiver-audio)
        message(FATAL_ERROR
            "AA_WIRED_E2E_REQUIRES_AUDIO_ROUTER: include cmake/AudioPipeline.cmake before cmake/WiredE2e.cmake")
    endif()
    add_executable(aa_wired_e2e_test ${AA_WIRED_TEST_SOURCES})
    set_target_properties(aa_wired_e2e_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(aa_wired_e2e_test PRIVATE
        aa_wired aa_protocol_adapter aap_protobuf aasdk
        aa_video aa_input aa_input_service aa_ipc_control aa_ipc_dbus aa_transport_tls
        aareceiver-audio pthread GTest::gtest_main)
    target_include_directories(aa_wired_e2e_test PRIVATE
        "${CMAKE_BINARY_DIR}/aasdk/protobuf"
        "${AA_DEPENDENCY_STAGE}/aasdk/include"
        ${Protobuf_INCLUDE_DIRS})
    if(TARGET PkgConfig::AA_AUDIO_GST)
        target_link_libraries(aa_wired_e2e_test PRIVATE PkgConfig::AA_AUDIO_GST)
    endif()
    target_compile_definitions(aa_wired_e2e_test PRIVATE AA_WIRED_HAS_AUDIO_ROUTER=1)
    target_link_options(aa_wired_e2e_test PRIVATE
        "-Wl,--wrap=fsync" "-Wl,--wrap=renameat"
        "-Wl,--wrap=_ZN2aa3app15open_wired_linkERKNS_9transport17UsbDeviceSelectorERKNS1_16AoaAccessoryInfoERKNS_5trust10TrustStoreERKNS_4core8DeadlineE"
        "-Wl,--wrap=_ZN2aa9transport28make_client_handshake_cipherERKSt8functionIFbvEE"
        "-Wl,--wrap=_ZN2aa5audio6detail17open_default_sinkERKNS1_15SinkOpenRequestE")
    list(APPEND AA_WIRED_WARN_TARGETS aa_wired_e2e_test)
endif()

foreach(target IN LISTS AA_WIRED_WARN_TARGETS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
endforeach()

if(AA_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR "AA_WIRED_E2E_ASAN_UNSUPPORTED: requires GNU or Clang")
    endif()
    foreach(_aa_wired_asan_target IN LISTS AA_WIRED_WARN_TARGETS)
        target_compile_options(${_aa_wired_asan_target} PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(${_aa_wired_asan_target} PRIVATE -fsanitize=address)
    endforeach()
    unset(_aa_wired_asan_target)
endif()

if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME wired_e2e_happy COMMAND aa_wired_e2e_test --gtest_filter=WiredE2eHappy.*)
    add_test(NAME wired_e2e_revoke COMMAND aa_wired_e2e_test --gtest_filter=WiredE2eRevoke.*)
    add_test(NAME wired_e2e_failures COMMAND aa_wired_e2e_test --gtest_filter=WiredE2eFailures.*)
    add_test(NAME wired_e2e_trust_link COMMAND aa_wired_e2e_test --gtest_filter=WiredE2eTrustLink.*)
    add_test(NAME wired_e2e_input_wire COMMAND aa_wired_e2e_test --gtest_filter=WiredE2eInputWire.*)
    add_test(NAME wired_e2e_auth COMMAND aa_wired_e2e_test --gtest_filter=WiredE2eAuth.*)
    add_test(NAME wired_e2e_av_sink COMMAND aa_wired_e2e_test --gtest_filter=WiredAvSink.*:WiredAvRegistration.*)
    set_tests_properties(wired_e2e_av_sink PROPERTIES TIMEOUT 120)
    add_test(NAME wired_e2e_control COMMAND aa_wired_e2e_test --gtest_filter=WiredControl.*)
    add_test(NAME wired_e2e_microphone COMMAND aa_wired_e2e_test --gtest_filter=WiredMicrophone.*)
    set_tests_properties(wired_e2e_control PROPERTIES TIMEOUT 120)
    add_test(NAME wired_recovery COMMAND aa_wired_e2e_test --gtest_filter=WiredRecovery.*)
    add_test(NAME wired_receiver COMMAND dbus-run-session --
        $<TARGET_FILE:aa_wired_e2e_test> --gtest_filter=WiredReceiver.*)
    add_test(NAME wired_deadline COMMAND aa_wired_e2e_test --gtest_filter=WiredDeadline.*)
    set_tests_properties(wired_deadline PROPERTIES TIMEOUT 30)
    set_tests_properties(wired_receiver PROPERTIES TIMEOUT 120)
    set_tests_properties(wired_e2e_happy wired_e2e_revoke wired_e2e_failures wired_e2e_trust_link
        wired_e2e_input_wire wired_e2e_auth wired_e2e_microphone wired_recovery PROPERTIES TIMEOUT 120)
    add_test(NAME wired_e2e_live_qa_gates COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/wired_e2e/test_live_qa_gates.py"
        "${CMAKE_SOURCE_DIR}/tools/wired-qa/run-wired-live-qa.sh")
    set_tests_properties(wired_e2e_live_qa_gates PROPERTIES TIMEOUT 120)
endif()

# Phase-B live receiver: the composed USB/AOA -> WiredSession -> video/input/
# audio/D-Bus entry point the wired-qa harness drives. NATIVE-ONLY today: the
# protocol codec it needs (aa_protocol_adapter) is native-only (the frozen
# cross sysroot has no AASDK development inputs, same boundary as
# cmake/ProtocolAdapter.cmake) - on-target packaging is todo 41.
if(NOT CMAKE_CROSSCOMPILING)
    set(AA_WIRED_RECEIVER_SOURCES tools/wired-receiver/main.cpp)
    foreach(source IN LISTS AA_WIRED_RECEIVER_SOURCES)
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}")
            message(FATAL_ERROR
                "AA_WIRED_SOURCE_MISSING: ${source} (wired receiver source inventory)")
        endif()
    endforeach()
    add_executable(aa-wired-receiver ${AA_WIRED_RECEIVER_SOURCES})
    set_target_properties(aa-wired-receiver PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(aa-wired-receiver PRIVATE
        aa_wired aa_protocol_adapter aa_ipc_control aa_ipc_dbus aa_video aa_input
        aareceiver-audio pthread)
    if(TARGET aa_transport_tls)
        target_link_libraries(aa-wired-receiver PRIVATE aa_transport_tls)
    endif()
    if(TARGET aap_protobuf AND TARGET aasdk)
        target_link_libraries(aa-wired-receiver PRIVATE aap_protobuf aasdk)
    endif()
    if(TARGET PkgConfig::AA_AUDIO_GST)
        target_link_libraries(aa-wired-receiver PRIVATE PkgConfig::AA_AUDIO_GST)
    endif()
    if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(aa-wired-receiver PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror)
    endif()
    if(AA_ENABLE_ASAN)
        target_compile_options(aa-wired-receiver PRIVATE
            -fsanitize=address -fno-omit-frame-pointer)
        target_link_options(aa-wired-receiver PRIVATE -fsanitize=address)
    endif()
    unset(AA_WIRED_RECEIVER_SOURCES)
endif()

unset(AA_WIRED_SOURCES)
unset(AA_WIRED_TEST_SOURCES)
unset(AA_WIRED_LINK)
unset(AA_WIRED_WARN_TARGETS)
