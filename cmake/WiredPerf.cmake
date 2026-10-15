# SPDX-License-Identifier: GPL-3.0-or-later
# Task-32 wired performance/soak tooling. Host-only fixture tests: schema
# validation, the nominal PASS fixture, the failing-budget fixture, strict
# duration parsing, the plan-mandated contracts, and the on-target collector
# driven against a deterministic fake receiver. The live 30m/2h measurement
# runs on the target under orchestrator control (phase B, see
# docs/qualification/wired-perf-runbook.md) and is NOT a ctest.
if(NOT CMAKE_CROSSCOMPILING)
    add_executable(aa_receiver_timing_test tests/perf/receiver_timing_test.cpp)
    set_target_properties(aa_receiver_timing_test PROPERTIES
        CXX_STANDARD 20 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    get_target_property(receiver_libraries aa-wired-receiver LINK_LIBRARIES)
    target_link_libraries(aa_receiver_timing_test PRIVATE ${receiver_libraries} GTest::gtest_main)
    add_test(NAME wired_perf_receiver_timing COMMAND aa_receiver_timing_test)
    add_test(NAME wired_perf_av_sync COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/perf/test_review_regressions.py" av_sync)
    add_test(NAME wired_perf_display_sources COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/tests/perf/test_review_regressions.py" display_sources)
    foreach(case schema nominal budget_fail duration contract collector media_timing readiness)
        add_test(NAME wired_perf_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/perf/test_wired_perf.py" ${case})
        set_tests_properties(wired_perf_${case} PROPERTIES TIMEOUT 180)
    endforeach()
endif()
