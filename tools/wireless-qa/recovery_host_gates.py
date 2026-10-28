# SPDX-License-Identifier: GPL-3.0-or-later
"""Existing task38 host timing and cleanup gates, shared by both schemas."""

HOST_SCENARIOS = tuple(f"R{number:02d}" for number in range(1, 12))
OFFSETS = (
    "admission_closed_ms", "link_retired_ms", "grace_expiry_observed_ms",
    "teardown_started_ms", "old_ap_inactive_ms", "fresh_acquisition_ms",
    "terminal_recovery_ms",
)
REQUIRED_OFFSETS = {
    "R01": {"admission_closed_ms", "link_retired_ms", "fresh_acquisition_ms", "terminal_recovery_ms"},
    "R02": {"admission_closed_ms", "link_retired_ms", "terminal_recovery_ms"},
    "R03": {"admission_closed_ms", "link_retired_ms", "teardown_started_ms", "old_ap_inactive_ms", "terminal_recovery_ms"},
    "R04": {"admission_closed_ms", "link_retired_ms", "teardown_started_ms", "old_ap_inactive_ms", "fresh_acquisition_ms", "terminal_recovery_ms"},
    "R05": {"admission_closed_ms", "link_retired_ms", "teardown_started_ms", "old_ap_inactive_ms", "fresh_acquisition_ms", "terminal_recovery_ms"},
    "R06": {"admission_closed_ms", "link_retired_ms", "teardown_started_ms", "old_ap_inactive_ms", "fresh_acquisition_ms", "terminal_recovery_ms"},
    "R07": {"admission_closed_ms", "link_retired_ms", "old_ap_inactive_ms", "fresh_acquisition_ms", "terminal_recovery_ms"},
    "R08": {"admission_closed_ms", "link_retired_ms", "grace_expiry_observed_ms", "teardown_started_ms", "old_ap_inactive_ms", "terminal_recovery_ms"},
    "R09": {"admission_closed_ms", "link_retired_ms", "teardown_started_ms", "old_ap_inactive_ms", "fresh_acquisition_ms", "terminal_recovery_ms"},
    "R10": {"terminal_recovery_ms"},
    "R11": {"admission_closed_ms", "link_retired_ms", "fresh_acquisition_ms", "terminal_recovery_ms"},
}
ACTIVATION_SCENARIOS = {"R01", "R04", "R05", "R06", "R07", "R09", "R11"}
NEW_ACTIVATION_SCENARIOS = {"R04", "R05", "R06", "R07", "R09"}
AP_OFF_SCENARIOS = {"R03", "R04", "R05", "R06", "R07", "R08", "R09"}


def valid_offset(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def validate_host(scenarios: dict) -> list[str]:
    errors = []
    for scenario in HOST_SCENARIOS:
        entry = scenarios[scenario]
        if entry["status"] not in {"pass", "fail", "blocked"}:
            errors.append(f"{scenario}: invalid status")
            continue
        if entry["status"] == "blocked" and scenario != "R07":
            errors.append(f"{scenario}: blocked outcome is not an approved exception")
        for field in OFFSETS:
            if field not in entry:
                errors.append(f"{scenario}: missing offset key {field}")
            elif entry[field] is None and field not in entry.get("null_reasons", {}):
                errors.append(f"{scenario}: null {field} has no applicability reason")
            elif entry[field] is not None and not valid_offset(entry[field]):
                errors.append(f"{scenario}: {field} must be a nonnegative integer")
        if entry["status"] == "fail":
            continue
        cases = entry.get("cases")
        if not isinstance(cases, list) or not cases:
            errors.append(f"{scenario}: missing per-case evidence")
            continue
        for field in REQUIRED_OFFSETS[scenario]:
            if cases[0].get("offsets", {}).get(field) is None:
                errors.append(f"{scenario}: primary case missing {field}")
        statuses = []
        for index, case in enumerate(cases):
            label = f"{scenario} case {index}"
            status, observations, offsets = case.get("status"), case.get("observations"), case.get("offsets")
            if status not in {"pass", "fail", "blocked"} or not isinstance(observations, dict) or not isinstance(offsets, dict):
                errors.append(f"{label}: invalid status, observations or offsets")
                continue
            statuses.append(status)
            for field in OFFSETS:
                if field not in offsets:
                    errors.append(f"{label}: missing offset key {field}")
                elif offsets[field] is not None and not valid_offset(offsets[field]):
                    errors.append(f"{label}: {field} must be a nonnegative integer")
            common = {"terminal_recovery_ms"} if scenario == "R10" else {
                "admission_closed_ms", "link_retired_ms", "terminal_recovery_ms"}
            for field in common:
                if offsets.get(field) is None:
                    errors.append(f"{label}: missing {field}")
            admission, link = offsets.get("admission_closed_ms"), offsets.get("link_retired_ms")
            terminal, inactive = offsets.get("terminal_recovery_ms"), offsets.get("old_ap_inactive_ms")
            teardown = offsets.get("teardown_started_ms")
            if scenario != "R10" and valid_offset(admission) and admission > 1000:
                errors.append(f"{label}: admission closure exceeds 1s")
            if scenario not in {"R08", "R10"} and valid_offset(link) and link > 5000:
                errors.append(f"{label}: link teardown exceeds 5s")
            if valid_offset(terminal) and terminal > (1000 if scenario == "R10" else 15000):
                errors.append(f"{label}: terminal state exceeds its budget")
            if scenario in AP_OFF_SCENARIOS and scenario != "R08" and valid_offset(inactive) and inactive > 5000:
                errors.append(f"{label}: old AP inactivity exceeds 5s")
            if scenario == "R08":
                for field in ("grace_expiry_observed_ms", "teardown_started_ms", "link_retired_ms"):
                    value = offsets.get(field)
                    if valid_offset(value) and not 10000 <= value < 11000:
                        errors.append(f"{label}: {field} outside [10000,11000)")
                if valid_offset(teardown) and valid_offset(inactive) and inactive > min(teardown + 5000, 15000):
                    errors.append(f"{label}: AP-off after derived deadline")
            cleanup = observations.get("hard_kill_cleanup", "")
            if cleanup.startswith("BLOCKED") and status != "blocked":
                errors.append(f"{label}: blocked cleanup case must have blocked status")
            if status == "blocked" and not cleanup.startswith("BLOCKED"):
                errors.append(f"{label}: blocked case lacks a blocked cleanup reason")
            if scenario in AP_OFF_SCENARIOS:
                for key in ("cleanup_nm", "cleanup_nft", "cleanup_bluez"):
                    if not observations.get(key):
                        errors.append(f"{label}: missing {key}")
            elif not observations.get("cleanup_observations_applicability"):
                errors.append(f"{label}: missing cleanup applicability reason")
            if observations.get("trust_store_unmodified") != "true":
                errors.append(f"{label}: trust-store preservation is not evidenced")
        if statuses:
            expected = "fail" if "fail" in statuses else "blocked" if "blocked" in statuses else "pass"
            if entry["status"] != expected:
                errors.append(f"{scenario}: aggregate status does not match per-case outcomes")
        usable = [case for case in cases if isinstance(case.get("offsets"), dict)]
        for field in OFFSETS:
            values = [case["offsets"][field] for case in usable if valid_offset(case["offsets"].get(field))]
            expected = max(values) if values else None
            if entry.get(field) != expected:
                errors.append(f"{scenario}: aggregate {field} does not match cases")
        if entry.get("protocol_elapsed_ms") != entry.get("terminal_recovery_ms"):
            errors.append(f"{scenario}: protocol_elapsed_ms does not match terminal recovery")
        for field in ("old_activation_id", "new_activation_id"):
            raw = cases[0].get("observations", {}).get(field)
            try:
                expected = int(raw) if raw is not None else None
            except ValueError:
                expected = None
                errors.append(f"{scenario}: invalid primary {field}")
            if entry.get(field) != expected:
                errors.append(f"{scenario}: aggregate {field} does not match primary case")
        expected_reason = next((case.get("observations", {}).get("hard_kill_cleanup") for case in cases
                                if case.get("observations", {}).get("hard_kill_cleanup", "").startswith("BLOCKED")), None)
        if entry.get("blocked_reason") != expected_reason:
            errors.append(f"{scenario}: blocked_reason does not match per-case cleanup evidence")
        for field in REQUIRED_OFFSETS[scenario]:
            if entry.get(field) is None:
                errors.append(f"{scenario}: missing required observation {field}")
        if any(entry.get(field) is None for field in REQUIRED_OFFSETS[scenario]):
            continue
        if scenario != "R10" and entry["admission_closed_ms"] > 1000:
            errors.append(f"{scenario}: admission closure exceeds 1s")
        if scenario not in {"R08", "R10"} and entry["link_retired_ms"] > 5000:
            errors.append(f"{scenario}: link teardown exceeds 5s")
        if entry["terminal_recovery_ms"] > (1000 if scenario == "R10" else 15000):
            errors.append(f"{scenario}: terminal state exceeds its budget")
        if scenario in AP_OFF_SCENARIOS and scenario != "R08" and entry["old_ap_inactive_ms"] > 5000:
            errors.append(f"{scenario}: old AP inactivity exceeds 5s")
        if scenario in ACTIVATION_SCENARIOS:
            old, new = entry.get("old_activation_id"), entry.get("new_activation_id")
            if old is None or new is None:
                errors.append(f"{scenario}: missing old/new activation distinction")
            elif scenario in NEW_ACTIVATION_SCENARIOS and new <= old:
                errors.append(f"{scenario}: recovery did not use a distinct new activation")
        if scenario == "R08":
            for field in ("grace_expiry_observed_ms", "teardown_started_ms", "link_retired_ms"):
                if not 10000 <= entry[field] < 11000:
                    errors.append(f"R08: {field} outside [10000,11000)")
            deadline = min(entry["teardown_started_ms"] + 5000, 15000)
            if entry["old_ap_inactive_ms"] > deadline:
                errors.append("R08: AP-off after derived deadline despite protocol result")
        if scenario == "R07":
            cases = entry.get("cases", [])
            cleanups = [case["observations"].get("hard_kill_cleanup", "") for case in cases]
            if len(cases) < 2 or not any(value.startswith("BLOCKED") for value in cleanups):
                errors.append("R07: unverified hard-kill cleanup must remain explicitly blocked")
            if entry["status"] != "blocked" or not entry.get("blocked_reason"):
                errors.append("R07: cleanup limitation must not be promoted to pass")
            if not all(case["observations"].get("protocol_result", "").startswith("pass:") for case in cases):
                errors.append("R07: protocol result must be recorded separately from cleanup")
        if scenario == "R11":
            observations = entry["cases"][0]["observations"] if entry.get("cases") else {}
            for key in ("fresh_confirmed_active", "video_delivered", "audio_delivered", "input_delivered"):
                if observations.get(key) != "true":
                    errors.append(f"R11: missing {key} evidence")
            if observations.get("admission") != "production activation-bound predicate":
                errors.append("R11: admission is not the production activation-bound predicate")
    return errors
