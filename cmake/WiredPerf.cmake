# SPDX-License-Identifier: GPL-3.0-or-later
# Task-32 wired performance/soak tooling. Host-only fixture tests: schema
# validation, the nominal PASS fixture, the failing-budget fixture, strict
# duration parsing, the plan-mandated contracts, and the on-target collector
# driven against a deterministic fake receiver. The live 30m/2h measurement
# runs on the target under orchestrator control (phase B, see
# docs/qualification/wired-perf-runbook.md) and is NOT a ctest.
if(NOT CMAKE_CROSSCOMPILING)
    foreach(case schema nominal budget_fail duration contract collector readiness)
        add_test(NAME wired_perf_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/perf/test_wired_perf.py" ${case})
        set_tests_properties(wired_perf_${case} PROPERTIES TIMEOUT 180)
    endforeach()
endif()
