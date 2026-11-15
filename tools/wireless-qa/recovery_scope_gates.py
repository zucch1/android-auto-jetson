# SPDX-License-Identifier: GPL-3.0-or-later
"""Versioned closure rules; legacy withdrawals are never silently migrated."""


def validate_scope(document: dict, required_withdrawals: list[str]) -> list[str]:
    errors = []
    version = document.get("schema_version", 1)
    if type(version) is not int or version not in (1, 2):
        return ["unsupported schema_version"]
    statuses = {"failed", "pending-owner-signoff", "completed-reduced-scope"}
    if version == 2:
        statuses.add("completed-full-scope")
        if document.get("task") != "task-38-wireless-recovery":
            errors.append("v2 requires task-38-wireless-recovery task identity")
        if document.get("qualification_scope") != "full-task38":
            errors.append("v2 requires qualification_scope full-task38")
    if document["compliance_status"] not in statuses:
        errors.append("invalid compliance_status")
    signoff = document["reduced_scope_signoff"]
    withdrawn = document["withdrawn_claims"]
    if not isinstance(withdrawn, list) or not all(isinstance(claim, str) for claim in withdrawn):
        return errors + ["withdrawn_claims must be a list of strings"]
    if version == 1:
        for claim in required_withdrawals:
            if claim not in withdrawn:
                errors.append(f"required withdrawn claim missing: {claim}")
    if document["compliance_status"] == "completed-reduced-scope":
        if not isinstance(signoff, dict) or not all(signoff.get(key) for key in ("approver", "date", "claims", "pr_head")):
            errors.append("reduced-scope closure requires explicit owner sign-off with approver/date/claims/pr_head")
        else:
            if signoff["pr_head"] != document["git_commit"] or document["git_commit"] == "unknown":
                errors.append("reduced-scope sign-off pr_head does not match evaluated git_commit")
            if document.get("worktree_dirty") is not False:
                errors.append("reduced-scope closure requires evidence from a verified clean worktree")
            claims = signoff["claims"]
            if not isinstance(claims, list):
                errors.append("reduced-scope sign-off claims must be a list")
            else:
                for claim in withdrawn:
                    if claim not in claims:
                        errors.append(f"reduced-scope sign-off does not cover withdrawn claim: {claim}")
    elif signoff is not None:
        errors.append("sign-off is present but closure is not completed-reduced-scope")
    approval = document["owner_approval"]
    if not isinstance(approval, dict) or not approval.get("decision") or not approval.get("date"):
        errors.append("missing bounded-cleanup owner decision/date")
    for scenario in ("B01", "B02", "B03"):
        entry = next(s for s in document["scenarios"] if s["scenario_id"] == scenario)
        if version == 1:
            if entry["status"] != "blocked" or not entry.get("blocked_reason"):
                errors.append(f"{scenario} must remain blocked with a reason")
            if not any(scenario in claim for claim in withdrawn):
                errors.append(f"{scenario} has no withdrawn claim")
    return errors
