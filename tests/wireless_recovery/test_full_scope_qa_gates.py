#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic schema tests ONLY: no fixture is real-phone qualification evidence."""
import copy
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from test_recovery_qa_gates import MODULE, qa, valid_document
from recovery_device_gates import DEVICE_IDS, COMMON_ARTIFACTS, EXTRA_ARTIFACTS


def synthetic_full_scope_document(root: Path) -> dict:
    """Build deliberately synthetic receipts solely to exercise schema success."""
    document = valid_document()
    document.update(schema_version=2, qualification_scope="full-task38", withdrawn_claims=[],
                    worktree_dirty=False, compliance_status="completed-full-scope", device_evidence={},
                    full_scope_signoff={"approver": "SYNTHETIC SCHEMA TEST ONLY", "date": "2026-10-28",
                                        "pr_head": document["git_commit"]})
    for name in DEVICE_IDS:
        observations = {
            "trust_store_unmodified": True, "identity_fence": True, "generation_fence": True,
            "owner_fence": True, "source_fence": True, "final_transmission_fence": True,
            "freshness_elapsed_ms": 900, "admission_closed_ms": 100,
            "terminal_recovery_ms": 500, "terminal_state": "ACTIVE", "fresh_bootstrap": True,
            "confirmed_active": True, "video_delivered": True, "audio_delivered": True,
            "input_delivered": True, "link_retired_ms": 100, "old_ap_inactive_ms": 200,
            "old_activation_id": 1, "new_activation_id": 2, "teardown_started_ms": 100,
        }
        if name == "baseline":
            observations.update(paired=True, bonded=True, authenticated_type5=True, hfp_present=True,
                                wpp_completed=True, ap_active=True, tcp_connected=True,
                                aap_authenticated=True, rfcomm_channel=8,
                                bootstrap_elapsed_ms=500, aap_auth_elapsed_ms=500)
        if name == "R08":
            observations.update(link_retired_ms=10020, grace_expiry_observed_ms=10010,
                                teardown_started_ms=10020, old_ap_inactive_ms=11000,
                                terminal_recovery_ms=12000)
        if name in {"B01", "B02", "B03"}:
            observations["stimulus"] = {"B01": "adb-lock-unlock", "B02": "adb-aa-restart",
                                       "B03": "bluetoothctl-disconnect-block"}[name]
        if name == "B03":
            observations.update(disconnect_exercised=True, block_exercised=True)
        if name == "R07":
            observations.update(independent_ap_inactive=True, owned_nft_removed=True,
                                bluez_cleanup_verified=True)
        if name in {"R10", "note9_rejection"}:
            observations.update(terminal_state="FAIL_CLOSED", admission_closed=True,
                                credentials_released=False, session_created=False, phone_persisted=False,
                                approved_phone=False)
        artifacts = {}
        for kind in COMMON_ARTIFACTS + EXTRA_ARTIFACTS.get(name, ()):
            content = f"SYNTHETIC SCHEMA TEST ONLY: {name}/{kind}\n".encode()
            path = root / f"{name}-{kind}.txt"
            path.write_bytes(content)
            artifacts[kind] = {"path": path.name, "sha256": hashlib.sha256(content).hexdigest()}
        document["device_evidence"][name] = {
            "status": "pass", "source": "on-device", "git_commit": document["git_commit"],
            "timestamp": "2026-10-28T00:00:00Z", "target_alias": "synthetic-target",
            "phone_alias": "Note9" if name == "note9_rejection" else "synthetic-approved-phone",
            "run_id": "SYNTHETIC-SCHEMA-TEST-ONLY", "observations": observations, "artifacts": artifacts,
        }
        if name == "R10":
            document["device_evidence"][name]["phone_alias"] = "synthetic-unknown-phone"
    for scenario in document["scenarios"]:
        if scenario["scenario_id"] in {"B01", "B02", "B03"}:
            scenario.update(status="pass", category="device", terminal_state="ACTIVE",
                            protocol_elapsed_ms=500, blocked_reason=None)
    return document


class FullScopeQaGates(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parent)
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.document = synthetic_full_scope_document(self.root)

    def assert_rejected(self, fragment):
        errors = qa.validate(self.document, self.root)
        self.assertTrue(any(fragment in error for error in errors), errors)

    def test_full_scope_success_when_synthetic_schema_receipts_complete(self):
        # Given: synthetic schema receipts only, never a device qualification result.
        # When: validate the complete schema fixture.
        errors = qa.validate(self.document, self.root)
        # Then: its structure is accepted without promoting host R07 cleanup.
        self.assertEqual(errors, [])
        self.assertEqual(self.document["hard_kill_cleanup"]["status"], "blocked")
        self.assertFalse(self.document["no_ap_leak_verified"])

    def test_completion_rejected_when_baseline_missing(self):
        del self.document["device_evidence"]["baseline"]
        self.assert_rejected("missing device evidence baseline")

    def test_completion_rejected_when_note9_rejection_missing(self):
        del self.document["device_evidence"]["note9_rejection"]
        self.assert_rejected("missing device evidence note9_rejection")

    def test_completion_rejected_when_target_scenario_missing(self):
        for name in (*qa.HOST_SCENARIOS, "B01", "B02", "B03"):
            with self.subTest(name=name):
                entry = self.document["device_evidence"].pop(name)
                self.assert_rejected(f"missing device evidence {name}")
                self.document["device_evidence"][name] = entry

    def test_completion_rejected_when_phone_stimulation_incomplete(self):
        for name in ("B01", "B02", "B03"):
            for status in ("pending", "blocked", "fail"):
                with self.subTest(name=name, status=status):
                    original = copy.deepcopy(self.document)
                    self.document["device_evidence"][name].update(status=status, reason="not qualified")
                    self.assert_rejected(f"{name}: full-scope completion requires pass")
                    self.document = original

    def test_completion_rejected_when_required_artifact_missing(self):
        for name in DEVICE_IDS:
            for kind in COMMON_ARTIFACTS + EXTRA_ARTIFACTS.get(name, ()):
                with self.subTest(name=name, kind=kind):
                    artifacts = self.document["device_evidence"][name]["artifacts"]
                    reference = artifacts.pop(kind)
                    self.assert_rejected(f"{name}: missing artifact {kind}")
                    artifacts[kind] = reference

    def test_completion_rejected_when_artifact_unavailable_or_changed(self):
        artifact = self.document["device_evidence"]["baseline"]["artifacts"]["hfp"]
        artifact["path"] = "missing.txt"
        self.assert_rejected("hfp: artifact unavailable")

    def test_completion_rejected_when_digest_mismatched(self):
        self.document["device_evidence"]["baseline"]["artifacts"]["hfp"]["sha256"] = "0" * 64
        self.assert_rejected("hfp: empty artifact or sha256 mismatch")

    def test_completion_rejected_when_artifact_escapes_evidence_directory(self):
        self.document["device_evidence"]["baseline"]["artifacts"]["hfp"]["path"] = "../outside.txt"
        self.assert_rejected("hfp: artifact escapes evidence directory")

    def test_completion_rejected_when_security_fence_missing(self):
        for field in ("identity_fence", "generation_fence", "owner_fence", "source_fence", "final_transmission_fence", "trust_store_unmodified"):
            with self.subTest(field=field):
                observations = self.document["device_evidence"]["B01"]["observations"]
                observations[field] = False
                self.assert_rejected(f"missing security evidence {field}")
                observations[field] = True

    def test_completion_rejected_when_b03_block_not_exercised(self):
        self.document["device_evidence"]["B03"]["observations"]["block_exercised"] = False
        self.assert_rejected("B03: missing block_exercised")

    def test_completion_rejected_when_hfp_or_projection_missing(self):
        for field in ("hfp_present", "wpp_completed", "confirmed_active", "authenticated_type5"):
            with self.subTest(field=field):
                observations = self.document["device_evidence"]["baseline"]["observations"]
                observations[field] = False
                self.assert_rejected(f"baseline: missing {field}")
                observations[field] = True

    def test_completion_rejected_when_freshness_or_admission_budget_relaxed(self):
        for field, value, error in (("freshness_elapsed_ms", 950, "freshness must be below 950ms"),
                                    ("admission_closed_ms", 1001, "over-budget admission_closed_ms")):
            with self.subTest(field=field):
                observations = self.document["device_evidence"]["B01"]["observations"]
                original = observations[field]
                observations[field] = value
                self.assert_rejected(error)
                observations[field] = original

    def test_completion_rejected_when_note9_gets_credentials(self):
        self.document["device_evidence"]["note9_rejection"]["observations"]["credentials_released"] = True
        self.assert_rejected("note9_rejection: credentials_released must be false")

    def test_completion_rejected_when_r07_cleanup_unverified(self):
        self.document["device_evidence"]["R07"].update(status="blocked", reason="cleanup-unverified")
        self.assert_rejected("R07: full-scope completion requires pass")

    def test_schema_accepts_verified_target_r07_fail_closed_without_new_activation(self):
        observations = self.document["device_evidence"]["R07"]["observations"]
        observations.update(terminal_state="FAIL_CLOSED", admission_closed=True, new_activation_id=None)
        self.assertEqual(qa.validate(self.document, self.root), [])

    def test_pending_and_failed_device_records_remain_representable(self):
        for status, compliance in (("pending", "pending-owner-signoff"), ("fail", "failed")):
            with self.subTest(status=status):
                document = valid_document()
                document.update(schema_version=2, qualification_scope="full-task38",
                                compliance_status=compliance, full_scope_signoff=None,
                                device_evidence={"baseline": {"status": status, "reason": "HFP unavailable"}})
                self.assertEqual(qa.validate(document, self.root), [])

    def test_completion_rejected_when_owner_signoff_missing(self):
        self.document["full_scope_signoff"] = None
        self.assert_rejected("full-scope completion requires explicit owner sign-off")

    def test_completion_rejected_when_evaluated_worktree_dirty(self):
        self.document["worktree_dirty"] = True
        self.assert_rejected("full-scope completion requires verified clean evaluated head")

    def test_completion_rejected_when_device_evidence_malformed(self):
        self.document.update(compliance_status="failed", device_evidence=[])
        self.assert_rejected("v2 requires device_evidence object")

    def test_legacy_completion_rejected_when_full_scope_claim_added(self):
        document = valid_document()
        document["compliance_status"] = "completed-full-scope"
        self.assertIn("invalid compliance_status", qa.validate(document, self.root))

    def test_cli_accepts_complete_schema_and_rejects_missing_baseline(self):
        path = self.root / "synthetic-schema-test-only.json"
        path.write_text(json.dumps(self.document))
        completed = subprocess.run([sys.executable, "-B", str(MODULE.resolve()), "validate", str(path)],
                                   capture_output=True, text=True, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        del self.document["device_evidence"]["baseline"]
        path.write_text(json.dumps(self.document))
        rejected = subprocess.run([sys.executable, "-B", str(MODULE.resolve()), "validate", str(path)],
                                  capture_output=True, text=True, check=False)
        self.assertEqual(rejected.returncode, 1, rejected.stdout + rejected.stderr)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
