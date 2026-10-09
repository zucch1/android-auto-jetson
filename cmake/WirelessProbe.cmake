# SPDX-License-Identifier: GPL-3.0-or-later
if(NOT CMAKE_CROSSCOMPILING)
    foreach(case policy discovery active cli window bluez)
        add_test(NAME wireless_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/wireless/test_probe.py" ${case})
        set_tests_properties(wireless_${case} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(case sections_fail_closed countries_and_rules_fail_closed
            restrictions_and_unknown_channels_fail_closed canonical_confirmed_us_reaches_active
            equivalent_channel_frequencies malformed_channel_frequencies_fail_closed
            cac_milliseconds_and_legacy_forms malformed_cac_units_fail_closed
            equivalent_formats_preserve_denials valid_self_managed_country00_overrides_global
            every_section_validated_with_cac_units retained_r2_inputs_remain_blocked)
        add_test(NAME wireless_regulatory_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/wireless/test_regulatory.py" Regulatory.test_${case})
        set_tests_properties(wireless_regulatory_${case} PROPERTIES TIMEOUT 30)
    endforeach()
    foreach(case permissions_and_owner_prevent_contact invalid_files_and_content_prevent_contact
            opened_inode_survives_path_substitution host_transfer_argv_and_report_are_secret_free
            nm_secret_stdin_and_failure_diagnostics nm_happy_argv_and_report_are_secret_free)
        add_test(NAME wireless_psk_${case} COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_SOURCE_DIR}/tests/wireless/test_psk.py" Psk.test_${case})
        set_tests_properties(wireless_psk_${case} PROPERTIES TIMEOUT 30)
    endforeach()
endif()
