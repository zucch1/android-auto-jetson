# SPDX-License-Identifier: GPL-3.0-or-later
# Applied after native test registration. Keep resource ownership visible here;
# private dbus-run-session buses, mkdtemp sockets and port-0 listeners need no lock.
# Composed wireless/recovery registrations retain aa_wireless_composed for the
# host-interface listener on TCP 5277 (WirelessMatrix/E2e/Recovery.cmake).
if(CMAKE_CROSSCOMPILING)
    return()
endif()
# Real elapsed-time bounds, unlike deterministic timestamps in protocol fixtures.
set(idle_host_tests
    input_sensors_memory
    video_socket_bench video_socket_backpressure video_socket_stream video_socket_ownership
    audio_pipeline_route audio_pipeline_decode audio_pipeline_sink
    wired_receiver wired_deadline wired_recovery
    tls_hardening wireless_e2e_attested wireless_e2e_composed_tls
    wireless_e2e_eligibility wireless_e2e_control_io wireless_e2e_tcp
    system_helper_nm_retry system_helper_nm_startup_private_bus system_helper_bootstrap
    usb_aoa_endpoint session_state_deadline media_decode_probe_suite)
get_property(host_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS host_tests)
    # These routes share the default user-session PipeWire/Pulse runtime.
    if(test MATCHES "^audio_pipeline_")
        set_property(TEST "${test}" APPEND PROPERTY RESOURCE_LOCK aa_session_audio)
    endif()
    # Wall-clock latency/throughput gates and memcheck require an idle test host,
    # not just exclusion against other gates. Pure fake-clock/schema tests remain parallel.
    if(test MATCHES "^wired_perf_" OR test IN_LIST idle_host_tests)
        set_tests_properties("${test}" PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()
