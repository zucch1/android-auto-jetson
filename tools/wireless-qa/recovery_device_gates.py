# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline task38 receipt checks, not an attestation of device qualification."""
from __future__ import annotations

import hashlib
from pathlib import Path
from typing import Final

from recovery_host_gates import (
    HOST_SCENARIOS, AP_OFF_SCENARIOS, ACTIVATION_SCENARIOS,
    NEW_ACTIVATION_SCENARIOS, valid_offset,
)

DEVICE_IDS: Final = ("baseline", *HOST_SCENARIOS, "B01", "B02", "B03", "note9_rejection")
COMMON_ARTIFACTS: Final = ("stimulus", "services", "bluez", "nm", "nft", "trust")
BASELINE_TRUE: Final = (
    "paired", "bonded", "authenticated_type5", "hfp_present", "wpp_completed",
    "ap_active", "tcp_connected", "aap_authenticated", "confirmed_active",
    "video_delivered", "audio_delivered", "input_delivered",
)
SECURITY_TRUE: Final = (
    "trust_store_unmodified", "identity_fence", "generation_fence", "owner_fence",
    "source_fence", "final_transmission_fence",
)
EXTRA_ARTIFACTS: Final = {
    "baseline": ("hfp", "rfcomm_wpp", "projection", "consumer"),
    "B01": ("adb", "projection"), "B02": ("adb", "projection"),
    "B03": ("bluetoothctl", "projection"), "R07": ("independent_ap_off",),
    "note9_rejection": ("rejection",),
}


def artifact_errors(artifacts: dict, root: Path, required: tuple[str, ...]) -> list[str]:
    errors: list[str] = []
    for kind in required:
        reference = artifacts.get(kind)
        if not isinstance(reference, dict):
            errors.append(f"missing artifact {kind}")
            continue
        relative, digest = reference.get("path"), reference.get("sha256")
        if not isinstance(relative, str) or not relative or Path(relative).is_absolute():
            errors.append(f"{kind}: artifact path must be relative to evidence directory")
            continue
        path = (root / relative).resolve()
        if not path.is_relative_to(root.resolve()):
            errors.append(f"{kind}: artifact escapes evidence directory")
            continue
        try:
            content = path.read_bytes()
        except OSError:
            errors.append(f"{kind}: artifact unavailable")
            continue
        if not content or hashlib.sha256(content).hexdigest() != digest:
            errors.append(f"{kind}: empty artifact or sha256 mismatch")
    return errors


def validate_device(document: dict, root: Path) -> list[str]:
    errors: list[str] = []
    records = document.get("device_evidence")
    if not isinstance(records, dict):
        return ["v2 requires device_evidence object (empty is allowed while pending)"]
    complete = document["compliance_status"] == "completed-full-scope"
    for name in DEVICE_IDS:
        entry = records.get(name)
        if entry is None:
            if complete:
                errors.append(f"missing device evidence {name}")
            continue
        if not isinstance(entry, dict) or entry.get("status") not in {"pending", "blocked", "fail", "pass"}:
            errors.append(f"{name}: invalid device evidence status")
            continue
        if entry["status"] != "pass":
            if not entry.get("reason"):
                errors.append(f"{name}: non-pass device evidence requires reason")
            if complete:
                errors.append(f"{name}: full-scope completion requires pass")
            continue
        for field in ("timestamp", "target_alias", "phone_alias", "run_id"):
            if not isinstance(entry.get(field), str) or not entry[field].strip():
                errors.append(f"{name}: missing {field}")
        if entry.get("source") != "on-device" or entry.get("git_commit") != document["git_commit"]:
            errors.append(f"{name}: requires on-device receipts for evaluated git_commit")
        baseline = records.get("baseline")
        if isinstance(baseline, dict) and name != "baseline":
            if entry.get("target_alias") != baseline.get("target_alias"):
                errors.append(f"{name}: target_alias does not match baseline")
            if name not in {"R10", "note9_rejection"} and entry.get("phone_alias") != baseline.get("phone_alias"):
                errors.append(f"{name}: approved phone_alias does not match baseline")
        observations = entry.get("observations")
        if not isinstance(observations, dict):
            errors.append(f"{name}: missing device observations")
            continue
        for field in SECURITY_TRUE:
            if observations.get(field) is not True:
                errors.append(f"{name}: missing security evidence {field}")
        age = observations.get("freshness_elapsed_ms")
        if not valid_offset(age) or age >= 950:
            errors.append(f"{name}: freshness must be below 950ms")
        if name == "baseline":
            for field in BASELINE_TRUE:
                if observations.get(field) is not True:
                    errors.append(f"baseline: missing {field}")
            if type(observations.get("rfcomm_channel")) is not int or observations["rfcomm_channel"] != 8:
                errors.append("baseline: requires RFCOMM channel 8")
            for field in ("bootstrap_elapsed_ms", "aap_auth_elapsed_ms"):
                if not valid_offset(observations.get(field)) or observations[field] > 10000:
                    errors.append(f"baseline: missing or over-budget {field}")
        else:
            for field, ceiling in (("admission_closed_ms", 1000), ("terminal_recovery_ms", 15000)):
                value = observations.get(field)
                if not valid_offset(value) or value > ceiling:
                    errors.append(f"{name}: missing or over-budget {field}")
            if observations.get("terminal_state") not in {"ACTIVE", "FAIL_CLOSED"}:
                errors.append(f"{name}: requires ACTIVE or FAIL_CLOSED terminal state")
            if observations.get("terminal_state") == "ACTIVE":
                for field in ("fresh_bootstrap", "confirmed_active", "video_delivered", "audio_delivered", "input_delivered"):
                    if observations.get(field) is not True:
                        errors.append(f"{name}: missing recovery evidence {field}")
            if observations.get("terminal_state") == "FAIL_CLOSED" and observations.get("admission_closed") is not True:
                errors.append(f"{name}: fail-closed admission is not evidenced")
        if name in (*HOST_SCENARIOS, "B01", "B02", "B03") and name != "R10":
            link = observations.get("link_retired_ms")
            if not valid_offset(link) or (name != "R08" and link > 5000):
                errors.append(f"{name}: missing or over-budget link_retired_ms")
        if name in AP_OFF_SCENARIOS:
            inactive = observations.get("old_ap_inactive_ms")
            deadline = 5000
            if name == "R08":
                for field in ("grace_expiry_observed_ms", "teardown_started_ms", "link_retired_ms"):
                    value = observations.get(field)
                    if not valid_offset(value) or not 10000 <= value < 11000:
                        errors.append(f"R08: device {field} outside [10000,11000)")
                start = observations.get("teardown_started_ms")
                deadline = min(start + 5000, 15000) if valid_offset(start) else 0
            if not valid_offset(inactive) or inactive > deadline:
                errors.append(f"{name}: missing or late device old_ap_inactive_ms")
            if name != "R07" and not valid_offset(observations.get("teardown_started_ms")):
                errors.append(f"{name}: missing device teardown_started_ms")
        if name in ACTIVATION_SCENARIOS:
            old, new = observations.get("old_activation_id"), observations.get("new_activation_id")
            recovering = observations.get("terminal_state") == "ACTIVE"
            if not valid_offset(old) or (recovering and (not valid_offset(new) or (name in NEW_ACTIVATION_SCENARIOS and new <= old))):
                errors.append(f"{name}: missing device activation distinction")
        if name == "R11" and observations.get("terminal_state") != "ACTIVE":
            errors.append("R11: device projection recovery must reach ACTIVE")
        expected_stimulus = {"B01": "adb-lock-unlock", "B02": "adb-aa-restart", "B03": "bluetoothctl-disconnect-block"}
        if name in expected_stimulus and observations.get("stimulus") != expected_stimulus[name]:
            errors.append(f"{name}: missing required device stimulus")
        if name == "B03":
            for field in ("disconnect_exercised", "block_exercised"):
                if observations.get(field) is not True:
                    errors.append(f"B03: missing {field}")
        if name == "R07":
            for field in ("independent_ap_inactive", "owned_nft_removed", "bluez_cleanup_verified"):
                if observations.get(field) is not True:
                    errors.append(f"R07: missing device cleanup {field}")
        if name in {"R10", "note9_rejection"}:
            if name == "note9_rejection" and entry.get("phone_alias") != "Note9":
                errors.append("note9_rejection: requires Note9 phone_alias")
            for field in ("credentials_released", "session_created", "phone_persisted"):
                if observations.get(field) is not False:
                    errors.append(f"{name}: {field} must be false")
            if observations.get("approved_phone") is not False:
                errors.append(f"{name}: requires an unknown/unapproved phone")
            if observations.get("terminal_state") != "FAIL_CLOSED" or not valid_offset(observations.get("terminal_recovery_ms")) or observations["terminal_recovery_ms"] > 1000:
                errors.append(f"{name}: rejection must fail closed within 1s")
        artifacts = entry.get("artifacts")
        if not isinstance(artifacts, dict):
            errors.append(f"{name}: missing artifacts")
        else:
            errors.extend(f"{name}: {error}" for error in artifact_errors(
                artifacts, root, COMMON_ARTIFACTS + EXTRA_ARTIFACTS.get(name, ())))
    for name in ("B01", "B02", "B03"):
        scenario = next(s for s in document["scenarios"] if s["scenario_id"] == name)
        record = records.get(name, {})
        if not isinstance(record, dict) or scenario.get("status") != record.get("status", "blocked"):
            errors.append(f"{name}: scenario status does not match device evidence")
        if scenario.get("status") == "blocked" and not scenario.get("blocked_reason"):
            errors.append(f"{name}: blocked scenario requires reason")
        if isinstance(record, dict) and record.get("status") == "pass":
            observations = record.get("observations", {})
            if not isinstance(observations, dict) or scenario.get("terminal_state") != observations.get("terminal_state") or scenario.get("protocol_elapsed_ms") != observations.get("terminal_recovery_ms"):
                errors.append(f"{name}: scenario terminal summary does not match device evidence")
    failed = any(isinstance(entry, dict) and entry.get("status") == "fail" for entry in records.values())
    if failed and document["compliance_status"] != "failed":
        errors.append("failed device evidence requires failed compliance_status")
    if document["compliance_status"] == "completed-reduced-scope":
        for name in DEVICE_IDS:
            record = records.get(name, {})
            if (not isinstance(record, dict) or record.get("status") != "pass") and not any(
                    name in claim for claim in document["withdrawn_claims"]):
                errors.append(f"{name}: reduced scope requires named withdrawal")
        if not any("R07" in claim for claim in document["withdrawn_claims"]):
            errors.append("R07: reduced scope requires host cleanup withdrawal")
    if complete:
        if document.get("withdrawn_claims"):
            errors.append("full-scope completion cannot withdraw required claims")
        if document.get("worktree_dirty") is not False or document["git_commit"] == "unknown":
            errors.append("full-scope completion requires verified clean evaluated head")
        signoff = document.get("full_scope_signoff")
        if not isinstance(signoff, dict) or not all(signoff.get(key) for key in ("approver", "date", "pr_head")):
            errors.append("full-scope completion requires explicit owner sign-off")
        elif signoff["pr_head"] != document["git_commit"]:
            errors.append("full-scope sign-off pr_head does not match evaluated git_commit")
    elif document.get("full_scope_signoff") is not None:
        errors.append("full-scope sign-off is present without completed-full-scope")
    return errors
