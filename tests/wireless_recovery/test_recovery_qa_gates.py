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
        cases = [case(f"{scenario}Primary", observations)]
        if scenario == "R07":
            observations.update({"hard_kill_cleanup": "pass: independent NM proof",
                                 "protocol_result": "pass: fresh admission"})
            blocked = {"trust_store_unmodified": "true", "admission_closed_ms": "100",
                       "link_retired_ms": "100", "terminal_recovery_ms": "100",
                       "old_activation_id": "1", "new_activation_id": "0",
                       "hard_kill_cleanup": "BLOCKED: cleanup-unverified / operator-supervised recovery required",
                       "protocol_result": "pass: fail-closed after respawn"}
            cases.append(case("R07Unverified", blocked))
        document["scenarios"].append(qa.scenario_from_cases(scenario, cases, 0))
    document["scenarios"].extend(qa.blocked_scenarios())
    document["trust_store_unmodified_on_reconnect"] = True
    document["no_ap_leak_verified"] = qa.expected_ap_leak(document)
    document["hard_kill_cleanup"] = {"status": "blocked", "reason": "cleanup-unverified"}
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

    def test_projection_recovery_requires_media_evidence(self):
        def remove_video(document):
            self.scenario(document, "R11")["cases"][0]["observations"]["video_delivered"] = "false"
        self.mutation(remove_video, "R11: missing video_delivered evidence")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
