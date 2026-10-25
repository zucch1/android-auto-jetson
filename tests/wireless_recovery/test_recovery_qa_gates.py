#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Negative controls for the task-38 host recovery evidence validator."""
import copy
import importlib.util
from pathlib import Path
import sys
import unittest

MODULE = Path(sys.argv[1]) if len(sys.argv) > 1 else (
    Path(__file__).resolve().parents[2] / "tools/wireless-qa/wireless_recovery_qa.py")
SPEC = importlib.util.spec_from_file_location("wireless_recovery_qa", MODULE)
qa = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(qa)


def case(name, observations):
    return {"name": name, "failed": False, "observations": observations}


def valid_document():
    document = qa.base_document()
    document["git_commit"] = "0123456789abcdef"
    for scenario in qa.HOST_SCENARIOS:
        observations = {"trust_store_unmodified": "true"}
        for field in qa.REQUIRED_OFFSETS[scenario]:
            observations[field] = "100"
        if scenario in qa.ACTIVATION_SCENARIOS:
            observations["old_activation_id"] = "1"
            observations["new_activation_id"] = "2" if scenario in qa.NEW_ACTIVATION_SCENARIOS else "1"
        if scenario == "R08":
            observations.update({"admission_closed_ms": "0", "link_retired_ms": "10081",
                                 "grace_expiry_observed_ms": "10081", "teardown_started_ms": "10016",
                                 "old_ap_inactive_ms": "10092", "terminal_recovery_ms": "10092"})
        if scenario == "R10":
            observations["terminal_recovery_ms"] = "6"
        if scenario == "R11":
            observations.update({"fresh_confirmed_active": "true", "video_delivered": "true",
                                 "audio_delivered": "true", "input_delivered": "true",
                                 "attestation": "test-only"})
        if scenario in qa.AP_OFF_SCENARIOS:
            observations.update({"cleanup_nm": "State4 and Device.ActiveConnection=/",
                                 "cleanup_nft": "owned table absent",
                                 "cleanup_bluez": "pairing closed; RFCOMM closed"})
        else:
            observations["cleanup_observations_applicability"] = "not applicable: no old-AP shutdown claim"
        cases = [case(f"{scenario}Primary", observations)]
        if scenario == "R07":
            observations.update({"hard_kill_cleanup": "pass: independent NM proof",
                                 "protocol_result": "pass: fresh admission"})
            blocked = {"trust_store_unmodified": "true", "admission_closed_ms": "100",
                       "link_retired_ms": "100", "terminal_recovery_ms": "100",
                       "old_activation_id": "1", "new_activation_id": "0",
                       "hard_kill_cleanup": "BLOCKED: cleanup-unverified / operator-supervised recovery required",
                       "protocol_result": "pass: fail-closed after respawn",
                       "cleanup_nm": "unknown: client-bound shutdown did not complete",
                       "cleanup_nft": "owned table retained; not AP-off proof",
                       "cleanup_bluez": "profile retained; RFCOMM closed"}
            cases.append(case("R07Unverified", blocked))
        document["scenarios"].append(qa.scenario_from_cases(scenario, cases, 0))
    document["scenarios"].extend(qa.blocked_scenarios())
    document["trust_store_unmodified_on_reconnect"] = True
    document["no_ap_leak_verified"] = qa.expected_ap_leak(document)
    r07 = next(entry for entry in document["scenarios"] if entry["scenario_id"] == "R07")
    document["hard_kill_cleanup"] = {"status": r07["status"], "reason": r07["blocked_reason"]}
    return document


class RecoveryQaGates(unittest.TestCase):
    def setUp(self):
        self.document = valid_document()
        self.assertEqual(qa.validate(self.document), [])

    def mutation(self, change, expected):
        document = copy.deepcopy(self.document)
        change(document)
        errors = qa.validate(document)
        self.assertTrue(any(expected in error for error in errors),
                        f"expected {expected!r} in {errors!r}")

    def scenario(self, document, scenario):
        return next(entry for entry in document["scenarios"] if entry["scenario_id"] == scenario)

    def test_missing_scenario(self):
        self.mutation(lambda d: d["scenarios"].remove(self.scenario(d, "R05")), "missing scenario R05")

    def test_missing_teardown_start(self):
        self.mutation(lambda d: self.scenario(d, "R03").update(teardown_started_ms=None),
                      "R03: missing required observation teardown_started_ms")

    def test_ap_off_after_derived_deadline(self):
        self.mutation(lambda d: self.scenario(d, "R08").update(old_ap_inactive_ms=15001),
                      "R08: AP-off after derived deadline")

    def test_missing_activation_distinction(self):
        self.mutation(lambda d: self.scenario(d, "R05").update(new_activation_id=1),
                      "R05: recovery did not use a distinct new activation")

    def test_unsupported_ap_leak_claim(self):
        self.mutation(lambda d: d.update(no_ap_leak_verified=True), "unsupported no_ap_leak_verified")

    def test_reduced_scope_requires_signoff(self):
        self.mutation(lambda d: d.update(compliance_status="completed-reduced-scope"),
                      "reduced-scope closure requires explicit owner sign-off")

    def test_blocked_device_scenario_cannot_pass(self):
        self.mutation(lambda d: self.scenario(d, "B01").update(status="pass"),
                      "B01 must remain blocked")

    def test_hard_kill_cleanup_cannot_be_promoted(self):
        def promote(document):
            entry = self.scenario(document, "R07")
            entry["status"] = "pass"
            entry["blocked_reason"] = None
        self.mutation(promote, "R07: cleanup limitation must not be promoted to pass")

    def test_missing_case_evidence(self):
        self.mutation(lambda d: self.scenario(d, "R06").update(cases=[]), "R06: missing per-case evidence")

    def test_blocked_case_cannot_be_promoted(self):
        def promote(document):
            self.scenario(document, "R07")["cases"][1]["status"] = "pass"
        self.mutation(promote, "R07 case 1: blocked cleanup case must have blocked status")

    def test_case_timing_cannot_hide_behind_aggregate(self):
        def late_case(document):
            self.scenario(document, "R07")["cases"][1]["offsets"]["terminal_recovery_ms"] = 15001
        self.mutation(late_case, "R07 case 1: terminal state exceeds its budget")

    def test_negative_offset_rejected(self):
        self.mutation(lambda d: self.scenario(d, "R03").update(admission_closed_ms=-1),
                      "R03: admission_closed_ms must be a nonnegative integer")

    def test_aggregate_must_match_cases(self):
        def change_aggregate(document):
            entry = self.scenario(document, "R06")
            entry["terminal_recovery_ms"] += 1
        self.mutation(change_aggregate, "R06: aggregate terminal_recovery_ms does not match cases")

    def test_cleanup_observation_required(self):
        def remove_cleanup(document):
            del self.scenario(document, "R03")["cases"][0]["observations"]["cleanup_nm"]
        self.mutation(remove_cleanup, "R03 case 0: missing cleanup_nm")

    def test_projection_recovery_requires_media_evidence(self):
        def remove_video(document):
            self.scenario(document, "R11")["cases"][0]["observations"]["video_delivered"] = "false"
        self.mutation(remove_video, "R11: missing video_delivered evidence")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
