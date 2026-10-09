#!/usr/bin/env bash
# arm64-cross-relay.sh — owner-machine relay for the private observed-sysroot cross smoke.
#
# Just-in-time usage: run this BEFORE each squash merge of the delivery chain. After a
# post-squash sync the head SHA changes, so the smoke must be re-run and its check-run
# re-posted for the new exact SHA — never relay a stale SHA.
#
# What it does at <head-sha>:
#   1. checks out that exact commit into a detached scratch worktree,
#   2. runs the OBSERVED-sysroot smoke (AA_SYSROOT_REQUIRE_OBSERVED=ON; payload bound to
#      the link sysroot) via `cmake --preset jetson-aarch64 && cmake --build --preset
#      jetson-aarch64`, capturing logs,
#   3. posts the `ci/arm64-cross-build` check-run for that exact SHA through the owner's
#      local gh channel, with real started/completed timestamps and sha256 of every log.
#
# The public `ci/arm64-cross-build` job auto-reports on every head in FIXTURE mode only
# (fixture-not-target). The observed smoke relayed here is the acceptance evidence
# (task 5 receipts / F3 later).
#
# Usage: tools/ci/arm64-cross-relay.sh <head-sha> [pr-number]
#
# Required environment (owner-local paths; never committed):
#   AA_SYSROOT_ROOTFS       observed sysroot rootfs payload root (sysroot jetson-r39.2.1)
#   AA_SYSROOT_MANIFEST     observed aa-sysroot-1 manifest.json paired with that rootfs
#   AA_GOOGLETEST_ARCHIVE   locked GoogleTest v1.15.2 archive
# Optional environment:
#   AA_SYSROOT_CACHE        immutable build cache root (default: scratch/build-binding)
#   AA_RELAY_SCRATCH        scratch root (default: ${TMPDIR:-/tmp}/arm64-cross-relay-<head-sha>)
#
# Zero secrets in this file: gh resolves credentials from the owner's own auth store, and
# no sysroot content or credential-bearing path is embedded here.
set -euo pipefail

usage() {
    echo "usage: $(basename "$0") <head-sha> [pr-number]" >&2
    exit 64
}

[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
head_sha=$1
pr_number=${2:-}

[[ "$head_sha" =~ ^[0-9a-f]{40}$ ]] || {
    echo "error: <head-sha> must be 40 lowercase hex characters" >&2
    exit 64
}
if [ -n "$pr_number" ]; then
    [[ "$pr_number" =~ ^[0-9]+$ ]] || {
        echo "error: [pr-number] must be decimal digits" >&2
        exit 64
    }
fi

for required in AA_SYSROOT_ROOTFS AA_SYSROOT_MANIFEST AA_GOOGLETEST_ARCHIVE; do
    [ -n "${!required:-}" ] || { echo "error: $required must be set in the environment" >&2; exit 64; }
done
[ -d "$AA_SYSROOT_ROOTFS" ] || { echo "error: AA_SYSROOT_ROOTFS is not a directory: $AA_SYSROOT_ROOTFS" >&2; exit 66; }
[ -f "$AA_SYSROOT_MANIFEST" ] || { echo "error: AA_SYSROOT_MANIFEST is not a file: $AA_SYSROOT_MANIFEST" >&2; exit 66; }
[ -f "$AA_GOOGLETEST_ARCHIVE" ] || { echo "error: AA_GOOGLETEST_ARCHIVE is not a file: $AA_GOOGLETEST_ARCHIVE" >&2; exit 66; }

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(git -C "$script_dir" rev-parse --show-toplevel)
scratch=${AA_RELAY_SCRATCH:-${TMPDIR:-/tmp}/arm64-cross-relay-$head_sha}
worktree=$scratch/worktree
logs=$scratch/logs
mkdir -p "$logs"
AA_SYSROOT_CACHE=${AA_SYSROOT_CACHE:-$scratch/build-binding}
mkdir -p "$AA_SYSROOT_CACHE"

git -C "$repo_root" fetch origin --quiet || echo "warning: git fetch origin failed; relying on local objects" >&2
git -C "$repo_root" cat-file -e "${head_sha}^{commit}" || {
    echo "error: commit $head_sha not present locally or on origin" >&2
    exit 66
}

git -C "$repo_root" worktree add --detach "$worktree" "$head_sha" >/dev/null
cleanup() { git -C "$repo_root" worktree remove --force "$worktree" >/dev/null 2>&1 || true; }
trap cleanup EXIT

# Observed-mode contract: the hash-validated payload IS the link sysroot (SysrootGate).
export AA_SYSROOT_ROOTFS AA_SYSROOT_MANIFEST AA_GOOGLETEST_ARCHIVE AA_SYSROOT_CACHE
export AA_SYSROOT_PAYLOAD=$AA_SYSROOT_ROOTFS
export AA_SYSROOT_REQUIRE_OBSERVED=ON
export PKG_CONFIG_PATH=

step_names=()
step_rcs=()
step_shas=()
overall_rc=0
run_step() {
    local name=$1
    shift
    local log=$logs/$name.log
    echo "relay: step=$name log=$log" >&2
    set +e
    (cd "$worktree" && "$@") >"$log" 2>&1
    local rc=$?
    set -e
    step_names+=("$name")
    step_rcs+=("$rc")
    step_shas+=("$(sha256sum "$log" | cut -d' ' -f1)")
    [ "$rc" -eq 0 ] || overall_rc=1
}

started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
run_step 00-require-observed python3 -B tools/build/validate_sysroot.py \
    --manifest "$AA_SYSROOT_MANIFEST" --payload "$AA_SYSROOT_ROOTFS" --require-observed
run_step 10-configure cmake --preset jetson-aarch64
run_step 20-build cmake --build --preset jetson-aarch64
completed_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)

if [ "$overall_rc" -eq 0 ]; then
    conclusion=success
    title="Observed-sysroot exact-SHA cross smoke PASS (sysroot jetson-r39.2.1)"
else
    conclusion=failure
    title="Observed-sysroot exact-SHA cross smoke FAIL (sysroot jetson-r39.2.1)"
fi

# Post the check-run with real timestamps and per-log sha256s; gh carries the credentials.
export RELAY_HEAD_SHA=$head_sha RELAY_PR_NUMBER=$pr_number RELAY_CONCLUSION=$conclusion
export RELAY_STARTED_AT=$started_at RELAY_COMPLETED_AT=$completed_at RELAY_TITLE=$title
export RELAY_LOGS=$logs RELAY_OVERALL_RC=$overall_rc
python3 -B - <<'PY' | gh api repos/zucch1/android-auto-jetson/check-runs --input -
import datetime
import hashlib
import json
import os
from pathlib import Path

logs = Path(os.environ['RELAY_LOGS'])
steps = []
text_parts = []
for log in sorted(logs.glob('*.log')):
    raw = log.read_bytes()
    steps.append(f'- `{log.name}` sha256 `{hashlib.sha256(raw).hexdigest()}` ({len(raw)} bytes)')
    tail = raw.decode('utf-8', 'replace').splitlines()[-20:]
    text_parts.append(f'### {log.name} (tail)\n```\n' + '\n'.join(tail) + '\n```')
pr = os.environ.get('RELAY_PR_NUMBER') or ''
summary = [
    f"Head `{os.environ['RELAY_HEAD_SHA']}` — observed-sysroot exact-SHA smoke "
    '(sysroot jetson-r39.2.1, AA_SYSROOT_REQUIRE_OBSERVED=ON).',
    'This is the acceptance evidence; the public ci/arm64-cross-build job is fixture '
    'mode (fixture-not-target) only (task 5 receipts / F3 later).',
    f"Relay run started {os.environ['RELAY_STARTED_AT']} / completed {os.environ['RELAY_COMPLETED_AT']} "
    f"({datetime.datetime.now(datetime.timezone.utc).isoformat()} relayed).",
]
if pr:
    summary.append(f'Pull request: https://github.com/zucch1/android-auto-jetson/pull/{pr}')
summary.append('Log sha256s:')
summary.extend(steps)
payload = {
    'name': 'ci/arm64-cross-build',
    'head_sha': os.environ['RELAY_HEAD_SHA'],
    'status': 'completed',
    'conclusion': os.environ['RELAY_CONCLUSION'],
    'started_at': os.environ['RELAY_STARTED_AT'],
    'completed_at': os.environ['RELAY_COMPLETED_AT'],
    'output': {
        'title': os.environ['RELAY_TITLE'],
        'summary': '\n'.join(summary),
        'text': '\n\n'.join(text_parts),
    },
}
if pr:
    payload['details_url'] = f'https://github.com/zucch1/android-auto-jetson/pull/{pr}'
print(json.dumps(payload))
PY

echo "relay: posted ci/arm64-cross-build for $head_sha conclusion=$conclusion" >&2
exit "$overall_rc"
