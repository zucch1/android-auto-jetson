#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Host task-38 recovery QA runner and fail-closed evidence validator.

Run:   wireless_recovery_qa.py run --build build/host-dev --out EVIDENCE.json
Check: wireless_recovery_qa.py validate EVIDENCE.json

The runner executes the registered ctest scenarios one at a time with
GTEST_OUTPUT, converts their recorded monotonic observations to JSON, then runs
the same validator used by the mutation tests. Blocked is never promoted to
pass, and a green host matrix alone never produces reduced-scope sign-off.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys
import xml.etree.ElementTree as ET

HOST_SCENARIOS = tuple(f"R{number:02d}" for number in range(1, 12))
BLOCKED_SCENARIOS = {
    "B01": "real phone lock/unlock via ADB is unavailable; no host surrogate claim",
    "B02": "real Android Auto restart via ADB is unavailable; no host surrogate claim",
    "B03": "real bluetoothctl disconnect/block and Jetson RF/driver behavior are unqualified",
}
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


def null_reason(scenario: str, field: str) -> str:
    if field == "grace_expiry_observed_ms":
        return "not applicable: no confirmed ACTIVE consumer grace in this scenario"
    if field == "fresh_acquisition_ms":
        return "not applicable: this scenario has no fresh AP acquisition"
    if field == "teardown_started_ms":
        if scenario == "R07":
            return "not applicable: SIGKILL has no graceful helper cleanup request"
        return "not applicable: no graceful helper cleanup request is claimed"
    if field == "old_ap_inactive_ms":
        return "not applicable: this scenario does not claim old-AP shutdown"
    if scenario == "R10":
        return "not applicable: synthetic second peer is rejected before admission"
    return "not applicable: observation is outside this scenario's terminal claim"


def parse_offsets(properties: dict[str, str]) -> dict[str, int | None]:
    result: dict[str, int | None] = {}
    for field in OFFSETS:
        value = properties.get(field)
        try:
            result[field] = int(value) if value is not None else None
        except ValueError:
            result[field] = None
    return result


def scenario_from_cases(scenario: str, cases: list[dict], returncode: int) -> dict:
    for case in cases:
        case["offsets"] = parse_offsets(case["observations"])
        cleanup = case["observations"].get("hard_kill_cleanup", "")
        case["status"] = "fail" if case.pop("failed") or returncode else (
            "blocked" if cleanup.startswith("BLOCKED") else "pass")
    status = "fail" if returncode or not cases or any(c["status"] == "fail" for c in cases) else (
        "blocked" if any(c["status"] == "blocked" for c in cases) else "pass")
    result = {
        "scenario_id": scenario,
        "category": "host",
        "status": status,
        "terminal_state": "; ".join(dict.fromkeys(
            value for case in cases for key in ("terminal_state", "protocol_result", "hard_kill_cleanup")
            if (value := case["observations"].get(key)))) or (
                "host assertions failed" if status == "fail" else "host assertions passed"),
        "protocol_elapsed_ms": None,
        "blocked_reason": next((case["observations"]["hard_kill_cleanup"] for case in cases
                                if case["observations"].get("hard_kill_cleanup", "").startswith("BLOCKED")), None),
        "cases": cases,
        "null_reasons": {},
    }
    for field in OFFSETS:
        values = [case["offsets"][field] for case in cases if case["offsets"][field] is not None]
        result[field] = max(values) if values else None
        if result[field] is None:
            result["null_reasons"][field] = null_reason(scenario, field)
    result["protocol_elapsed_ms"] = result["terminal_recovery_ms"]
    for field in ("old_activation_id", "new_activation_id"):
        values = [int(case["observations"][field]) for case in cases if field in case["observations"]]
        result[field] = values[0] if values else None
    return result


def failed_scenario(scenario: str, reason: str) -> dict:
    result = scenario_from_cases(scenario, [], 1)
    result["terminal_state"] = reason
    result["protocol_elapsed_ms"] = None
    return result


def blocked_scenarios() -> list[dict]:
    return [{
        "scenario_id": scenario,
        "category": "permanently-blocked-device",
        "status": "blocked",
        "terminal_state": "not executed; withdrawn claim recorded",
        "protocol_elapsed_ms": None,
        "blocked_reason": reason,
        **{field: None for field in OFFSETS},
        "null_reasons": {field: "not applicable: permanently blocked device scenario" for field in OFFSETS},
        "old_activation_id": None,
        "new_activation_id": None,
        "cases": [],
    } for scenario, reason in BLOCKED_SCENARIOS.items()]


def base_document() -> dict:
    return {
        "task": "task-38-wireless-recovery",
        "git_commit": "unknown",
        "timestamp": dt.datetime.now(dt.timezone.utc).isoformat(),
        "compliance_status": "pending-owner-signoff",
        "withdrawn_claims": [
            *(f"{scenario}: {reason}" for scenario, reason in BLOCKED_SCENARIOS.items()),
            "production TCP-to-Bluetooth identity binding remains BLOCKED; test attestation is not identity proof",
            "host NM/BlueZ doubles do not qualify real BlueZ signal completeness, RF, driver channel policy, or installed-service behavior",
            "no crash-durable managed-peer/revocation-ledger claim and no full wireless compliance claim",
        ],
        "owner_approval": {
            "decision": "bounded cleanup (option 1)",
            "date": "2026-10-24",
            "v2_7": "serialized-freshness clarification, owner option A, 2026-10-25",
        },
        "reduced_scope_signoff": None,
        "no_ap_leak_verified": False,
        "trust_store_unmodified_on_reconnect": False,
        "hard_kill_cleanup": {"status": "blocked", "reason": BLOCKED_SCENARIOS["B01"]},
        "scenarios": [],
    }


def run_matrix(build: Path, out: Path, repo: Path) -> dict:
    logs = (out.parent / f"{out.stem}-logs").resolve()
    logs.mkdir(parents=True, exist_ok=True)
    document = base_document()
    try:
        document["git_commit"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
        document["worktree_dirty"] = bool(subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=repo, text=True).strip())
    except (OSError, subprocess.CalledProcessError):
        document["worktree_dirty"] = None
    host = []
    for scenario in HOST_SCENARIOS:
        xml = logs / f"{scenario}.xml"
        log = logs / f"{scenario}.log"
        xml.unlink(missing_ok=True)
        environment = dict(os.environ, GTEST_OUTPUT=f"xml:{xml}")
        completed = subprocess.run(
            ["ctest", "--test-dir", str(build), "-R", f"^wireless_recovery_{scenario}$", "--output-on-failure"],
            env=environment, capture_output=True, text=True, check=False)
        log.write_text(completed.stdout + completed.stderr)
        cases = []
        if xml.exists():
            for case in ET.parse(xml).getroot().iter("testcase"):
                properties = {prop.attrib["name"]: prop.attrib["value"]
                              for prop in case.findall("./properties/property")}
                cases.append({"name": case.attrib.get("name", "unknown"),
                              "failed": bool(case.findall("failure") or case.findall("error")),
                              "observations": properties})
        host.append(scenario_from_cases(scenario, cases, completed.returncode) if cases
                    else failed_scenario(scenario, "ctest failed or emitted no gtest XML; see scenario log"))
    document["scenarios"] = host + blocked_scenarios()
    trust = [case["observations"].get("trust_store_unmodified") for scenario in host for case in scenario["cases"]]
    document["trust_store_unmodified_on_reconnect"] = bool(trust) and all(value == "true" for value in trust)
    document["no_ap_leak_verified"] = expected_ap_leak(document)
    r07 = next(scenario for scenario in host if scenario["scenario_id"] == "R07")
    document["hard_kill_cleanup"] = {
        "status": r07["status"], "reason": r07["blocked_reason"],
        "terminal_state": r07["terminal_state"],
    }
    if any(scenario["status"] == "fail" for scenario in host):
        document["compliance_status"] = "failed"
    return document


def expected_ap_leak(document: dict) -> bool:
    host = [s for s in document["scenarios"] if s["scenario_id"] in HOST_SCENARIOS]
    return all(s["status"] == "pass" for s in host) and all(
        s["old_ap_inactive_ms"] is not None for s in host if s["scenario_id"] in AP_OFF_SCENARIOS)


def validate(document: dict) -> list[str]:
    errors = []
    for key in ("task", "git_commit", "timestamp", "compliance_status", "withdrawn_claims",
                "owner_approval", "reduced_scope_signoff", "no_ap_leak_verified",
                "trust_store_unmodified_on_reconnect", "scenarios"):
        if key not in document:
            errors.append(f"missing top-level field {key}")
    if errors:
        return errors
    scenarios = {s.get("scenario_id"): s for s in document["scenarios"]}
    for scenario in (*HOST_SCENARIOS, *BLOCKED_SCENARIOS):
        if scenario not in scenarios:
            errors.append(f"missing scenario {scenario}")
    if errors:
        return errors
    if document["compliance_status"] not in {"failed", "pending-owner-signoff", "completed-reduced-scope"}:
        errors.append("invalid compliance_status")
    signoff = document["reduced_scope_signoff"]
    if document["compliance_status"] == "completed-reduced-scope":
        if not isinstance(signoff, dict) or not all(signoff.get(key) for key in ("approver", "date", "claims", "pr_head")):
            errors.append("reduced-scope closure requires explicit owner sign-off with approver/date/claims/pr_head")
    elif signoff is not None:
        errors.append("sign-off is present but closure is not completed-reduced-scope")
    approval = document["owner_approval"]
    if not isinstance(approval, dict) or not approval.get("decision") or not approval.get("date"):
        errors.append("missing bounded-cleanup owner decision/date")
    withdrawn = document["withdrawn_claims"]
    for scenario in BLOCKED_SCENARIOS:
        entry = scenarios[scenario]
        if entry["status"] != "blocked" or not entry.get("blocked_reason"):
            errors.append(f"{scenario} must remain blocked with a reason")
        if not any(scenario in claim for claim in withdrawn):
            errors.append(f"{scenario} has no withdrawn claim")
    for scenario in HOST_SCENARIOS:
        entry = scenarios[scenario]
        if entry["status"] not in {"pass", "fail", "blocked"}:
            errors.append(f"{scenario}: invalid status")
            continue
        for field in OFFSETS:
            if field not in entry:
                errors.append(f"{scenario}: missing offset key {field}")
            elif entry[field] is None and field not in entry.get("null_reasons", {}):
                errors.append(f"{scenario}: null {field} has no applicability reason")
        if entry["status"] == "fail":
            continue
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
            if "test-only" not in observations.get("attestation", ""):
                errors.append("R11: attestation scope is not explicit")
        for case in entry.get("cases", []):
            if case["observations"].get("trust_store_unmodified") != "true":
                errors.append(f"{scenario}: trust-store preservation is not evidenced")
    if any(scenarios[s]["status"] == "fail" for s in HOST_SCENARIOS):
        if document["compliance_status"] != "failed":
            errors.append("failed host scenario requires failed compliance_status")
    elif document["compliance_status"] == "failed":
        errors.append("failed compliance_status has no failed host scenario")
    if document["no_ap_leak_verified"] != expected_ap_leak(document):
        errors.append("unsupported no_ap_leak_verified value")
    if not document["trust_store_unmodified_on_reconnect"]:
        errors.append("trust_store_unmodified_on_reconnect is not evidenced")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("--build", type=Path, required=True)
    run.add_argument("--out", type=Path, required=True)
    run.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[2])
    check = sub.add_parser("validate")
    check.add_argument("evidence", type=Path)
    args = parser.parse_args()
    if args.command == "run":
        args.out.parent.mkdir(parents=True, exist_ok=True)
        document = run_matrix(args.build, args.out, args.repo)
        args.out.write_text(json.dumps(document, indent=2) + "\n")
        problems = validate(document)
    else:
        problems = validate(json.loads(args.evidence.read_text()))
    for problem in problems:
        print(f"FAIL {problem}")
    if problems:
        return 1
    print(f"PASS validated {args.out if args.command == 'run' else args.evidence}")
    return 1 if args.command == "run" and document["compliance_status"] == "failed" else 0


if __name__ == "__main__":
    raise SystemExit(main())
