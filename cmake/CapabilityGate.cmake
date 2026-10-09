# SPDX-License-Identifier: GPL-3.0-or-later
# Task 13 fail-fast capability gate: real CLI tests over tools/gates/check_capability.py.
# stdlib-Python only, so registration is unconditional in host and cross presets.
# Every test below runs the actual check_capability.py CLI in a subprocess.
set(_aa_gate_test tests/gates/test_check_capability.py)

# Whole-group runs (fast self-check of every case in the group).
foreach(group report fixtures failures)
    add_test(NAME capability_gate_${group} COMMAND "${Python3_EXECUTABLE}" -B
        "${CMAKE_SOURCE_DIR}/${_aa_gate_test}" ${group})
    set_tests_properties(capability_gate_${group} PROPERTIES TIMEOUT 120)
endforeach()

# Named acceptance-criteria and QA-scenario cases, one CTest test each.
add_test(NAME capability_gate_report_continuation
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Report.test_report_continuation_exit2)
add_test(NAME capability_gate_report_final_release
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Report.test_report_final_release_exit1)
add_test(NAME capability_gate_report_redacted
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Report.test_report_is_redacted_and_self_contained)
add_test(NAME capability_gate_good_provisional
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Fixtures.test_good_provisional_exit0)
add_test(NAME capability_gate_good_reducedscope
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Fixtures.test_good_reducedscope_exit2)
add_test(NAME capability_gate_missing_decode
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_missing_decode_exit1)
add_test(NAME capability_gate_signoff_failure
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_signoff_missing_exit1)
add_test(NAME capability_gate_release_stage_failure
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_release_stage_failure_exit1)
add_test(NAME capability_gate_budget_weakening
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_budget_weakening_exit1)
add_test(NAME capability_gate_bad_numeric
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_bad_numeric_string_exit1)
add_test(NAME capability_gate_nonfinite
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_nonfinite_exit1)
add_test(NAME capability_gate_false_passed_unmeasured
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_false_passed_unmeasured_exit1)
add_test(NAME capability_gate_unsigned_budget_fail
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_unsigned_budget_fail_exit1)
add_test(NAME capability_gate_blocker
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_blocker_exit1)
add_test(NAME capability_gate_false_full_pass
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_false_full_pass_unmeasured_release_exit1)
add_test(NAME capability_gate_missing_phone_fields
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_missing_phone_fields_exit1)
add_test(NAME capability_gate_inconsistent_measured
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Failures.test_inconsistent_duplicate_measured_exit1)
add_test(NAME capability_gate_isolated_tree
    COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_SOURCE_DIR}/${_aa_gate_test}"
        Report.test_checker_self_contained_in_isolated_tree)
foreach(_aa_gate_named
        capability_gate_report_continuation capability_gate_report_final_release
        capability_gate_report_redacted capability_gate_good_provisional
        capability_gate_good_reducedscope capability_gate_missing_decode
        capability_gate_signoff_failure capability_gate_release_stage_failure
        capability_gate_budget_weakening capability_gate_bad_numeric
        capability_gate_nonfinite capability_gate_false_passed_unmeasured
        capability_gate_unsigned_budget_fail capability_gate_blocker
        capability_gate_false_full_pass capability_gate_missing_phone_fields
        capability_gate_inconsistent_measured capability_gate_isolated_tree)
    set_tests_properties(${_aa_gate_named} PROPERTIES TIMEOUT 120)
endforeach()
unset(_aa_gate_named)
unset(_aa_gate_test)
