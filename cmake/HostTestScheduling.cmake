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
    wired_receiver wired_deadline wired_recovery wired_e2e_idr_io
    tls_hardening wireless_e2e_attested wireless_e2e_composed_tls
    wireless_e2e_eligibility wireless_e2e_control_io wireless_e2e_tcp
    system_helper_nm_retry system_helper_nm_startup_private_bus system_helper_bootstrap
    usb_aoa_endpoint session_state_deadline media_decode_probe_suite
    # Sub-2 s wall-clock bounds found in review of 1079736.
    dbus_contract_boundary system_helper_unit dummy_display_contract_render
    dummy_display_startup_readiness wireless_e2e_production wireless_e2e_composed_coordinator
    target_safety_diag_writer)
get_property(host_tests DIRECTORY PROPERTY TESTS)
foreach(test IN LISTS host_tests)
    # These routes share the default user-session PipeWire/Pulse runtime.
    if(test MATCHES "^audio_pipeline_")
        set_property(TEST "${test}" APPEND PROPERTY RESOURCE_LOCK aa_session_audio)
    endif()
    # Wall-clock latency/throughput gates and memcheck require an idle test host,
    # not just exclusion against other gates. Pure fake-clock/schema tests remain parallel.
    # Helper suites with 1-1.15 s response bounds or a 59-62 s expiry window. The enrolment
    # retirement suite drives the same ComposedSurface (NM double, mock BlueZ, ~1 s bounds).
    # aa_wireless_composed only excludes other holders of that lock; these need
    # an idle host, so they also run alone.
    # BusClient::confirmed()'s 1000 ms budget also bounds the NM AP private-bus, lifecycle,
    # composed, and helper matrix suites; the security suite asserts the 500 ms / 1 s helper
    # deadlines under slow, failing BlueZ removals (#118); the two-bus acceptance suite drives
    # confirmed() through real dbus-daemons and measures receiver-loss cleanup under a 4 s flood;
    # and wireless_recovery_* (excluding the pure-Python QA gate tests) assert 500-1000 ms
    # wall-clock bounds and R08's [9000,9500)/[10000,11000) windows (#177). Even with #176's
    # split DWARF reducing per-call helper hashing, these suites keep real wall-clock budgets
    # and already serialize against each other via aa_wireless_composed, so RUN_SERIAL protects
    # them from unrelated parallel CPU load at minimal runtime cost.
    # Both the system_helper_* and wireless_e2e_helper_* aliases of the same binaries/filters
    # (snapshot/nm_ap_private_bus, lifecycle, authority/unit, bootstrap, plus the pre-existing
    # retirement/bluez/composition pairs) are kept RUN_SERIAL: ctest --preset host-dev runs
    # both registrations, so leaving either alias parallel would expose the same 250-1000 ms
    # wall-clock budgets to parallel load. The duplicate registrations themselves are retained
    # so `ctest -R wireless_e2e` and `ctest -R system_helper` each remain self-contained.
    if(test MATCHES "^(system_helper_retirement_budget_|wireless_e2e_retirement_|system_helper_bluez_lifecycle_|system_helper_lifecycle_)" OR
       test MATCHES "^wireless_e2e_composed_" OR
       (test MATCHES "^wireless_recovery_" AND NOT test MATCHES "_qa_gates$") OR
       test MATCHES "^(system_helper_bluez_runtime|wireless_e2e_helper_bluez|system_helper_composed_runtime|wireless_e2e_helper_composition|system_helper_enrolment_retirement)$" OR
       test MATCHES "^wireless_e2e_helper_(snapshot|CompletedLifecycle|CompletedLoss|CompletedApLoss|FailedReplacement|authority|bootstrap)$" OR
       test MATCHES "^(system_helper_nm_ap_private_bus|system_helper_security|system_helper_private_bus)$")
        set_tests_properties("${test}" PROPERTIES RUN_SERIAL TRUE)
    endif()
    # host-asan intentionally keeps DWARF inline (not split) so ASan stack traces stay
    # symbolized even without llvm-symbolizer (#177). Serialize the remaining helper-hashing
    # suites under ASan so parallel load cannot push unsplit ASan executable SHA-256 hashing
    # past the 500 ms helper identity deadline.
    if(AA_ENABLE_ASAN AND
       test MATCHES "^(system_helper_delivery_policy|system_helper_bond_restart|wireless_receiver_helper_startup)$")
        set_tests_properties("${test}" PROPERTIES RUN_SERIAL TRUE)
    endif()
    # Target-safety guards watch every ancestor of their /tmp/opencode fixtures
    # and block on any identity change of /tmp itself (ancestor_setup_race in
    # tools/target_safety/coverage.py). Any parallel test creating a file in
    # /tmp changes it, so these must run alone.
    if(test MATCHES "^target_safety_")
        set_tests_properties("${test}" PROPERTIES RUN_SERIAL TRUE)
    endif()
    if(test MATCHES "^wired_perf_" OR test IN_LIST idle_host_tests)
        set_tests_properties("${test}" PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()
