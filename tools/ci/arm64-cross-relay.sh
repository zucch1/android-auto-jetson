#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# arm64-cross-relay.sh — owner-machine relay for the private observed-sysroot cross smoke.
#
# Trust model (security findings H1/M5, issue #89): the PR tree is UNTRUSTED code. This
# trusted script runs from the owner's own checkout of main (never from the PR branch) and:
#   1. fetches origin/main into a BASE worktree and runs the sysroot validator from there,
#      with a scrubbed environment, against the owner-local sysroot paths,
#   2. exports the exact PR head SHA into a scratch tree with tools/ci/relay-export-tree.py:
#      raw blobs from `git ls-tree`/`git cat-file`, NO attribute processing (a PR's
#      .gitattributes must not be able to select a filter or export-subst that runs on the
#      owner's machine), symlinks never followed, no .git link back to the owner's repository,
#   3. builds that tree ONLY inside a sandbox (podman/docker container or bubblewrap): no
#      $HOME, no gh auth, no SSH agent, no network, no inherited file descriptors, sysroot/
#      archive mounted read-only (the run is refused if a writable path overlaps one), and a
#      complete, scrubbed environment (fixed PATH, GIT_CONFIG_NOSYSTEM=1, empty HOME). A
#      container is killed, removed and verified gone before anything else happens; if that
#      cannot be confirmed nothing is posted and the relay exits 70,
#   4. only AFTER the sandbox has exited, posts the `ci/arm64-cross-build` COMMIT STATUS for
#      that exact SHA through the owner's local gh channel (outside the sandbox), with the
#      sysroot identity and short sha256s of every log.
# If no sandbox backend is usable the script stops; there is no unsandboxed mode.
# The relay attests that the PR tree builds against the observed sysroot. It does not make
# an unreviewed PR safe to merge: review the diff first. By default only PRs authored by the
# repository owner are relayed (see --allow-author).
#
# Just-in-time usage: run this BEFORE each squash merge of the delivery chain. After a
# post-squash sync the head SHA changes, so the smoke must be re-run and its status
# re-posted for the new exact SHA — never relay a stale SHA.
#
# GitHub check-run creation requires GitHub App auth (HTTP 403 for user tokens). This
# helper therefore posts a commit status — the same task-7 relay mechanism recorded in
# delivery-arm64-relay.json. Ruleset 24193601 accepts it by context name; arm64 has no
# integration_id bound to the required check.
#
# The public Actions job is named `ci/arm64-cross-fixture` and reports FIXTURE mode only
# (fixture-not-target). Only this relay posts `ci/arm64-cross-build`, so that required
# context always means an observed-sysroot result produced here.
#
# Usage: tools/ci/arm64-cross-relay.sh [--sandbox auto|podman|bwrap|docker]
#            [--allow-author LOGIN]... <head-sha> [pr-number]
#   Run it from your own checkout of main. With a pr-number the script checks, before building,
#   that <head-sha> is that PR's head and that the PR author is the repository owner or an
#   --allow-author login. Without a pr-number that binding is NOT checked (a warning is logged).
#
# Required environment (owner-local paths; never committed):
#   AA_SYSROOT_ROOTFS       observed sysroot rootfs payload root (sysroot jetson-r39.2.1)
#   AA_SYSROOT_MANIFEST     observed aa-sysroot-1 manifest.json paired with that rootfs
#   AA_GOOGLETEST_ARCHIVE   locked GoogleTest v1.15.2 archive
# Optional environment:
#   AA_SYSROOT_CACHE        immutable build cache root (default: scratch/build-binding)
#   AA_RELAY_SCRATCH        scratch root (default: ${TMPDIR:-/tmp}/arm64-cross-relay-<head-sha>)
#   AA_RELAY_SANDBOX        same as --sandbox
#   AA_RELAY_IMAGE          local container image (cmake, ninja, aarch64 g++, python3) for podman/docker
#   AA_RELAY_EXTRA_RO       colon-separated extra host paths to expose read-only to bwrap/containers
#                           (e.g. a toolchain installed outside /usr)
#   AA_RELAY_STEP_TIMEOUT   seconds allowed per sandboxed step (default 3600)
#
# Zero secrets in this file: gh resolves credentials from the owner's own auth store, and
# no sysroot content or credential-bearing path is embedded here.
set -euo pipefail

repo_slug=zucch1/android-auto-jetson
repo_owner=${repo_slug%%/*}
script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# shellcheck source=tools/ci/relay-sandbox-lib.sh
. "$script_dir/relay-sandbox-lib.sh"

usage() {
    echo "usage: $(basename "$0") [--sandbox auto|podman|bwrap|docker] [--allow-author LOGIN]... <head-sha> [pr-number]" >&2
    exit 64
}

sandbox_request=${AA_RELAY_SANDBOX:-auto}
allowed_authors=("$repo_owner")
positional=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --sandbox) [ "$#" -ge 2 ] || usage; sandbox_request=$2; shift 2 ;;
        --allow-author) [ "$#" -ge 2 ] || usage; allowed_authors+=("$2"); shift 2 ;;
        -h | --help) usage ;;
        --) shift; positional+=("$@"); break ;;
        -*) echo "error: unknown option $1" >&2; usage ;;
        *) positional+=("$1"); shift ;;
    esac
done
set -- "${positional[@]}"
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
case "$sandbox_request" in auto | podman | bwrap | docker) ;; *)
    echo "error: --sandbox must be auto, podman, bwrap or docker" >&2
    exit 64 ;;
esac

for required in AA_SYSROOT_ROOTFS AA_SYSROOT_MANIFEST AA_GOOGLETEST_ARCHIVE; do
    [ -n "${!required:-}" ] || { echo "error: $required must be set in the environment" >&2; exit 64; }
done
[ -d "$AA_SYSROOT_ROOTFS" ] || { echo "error: AA_SYSROOT_ROOTFS is not a directory: $AA_SYSROOT_ROOTFS" >&2; exit 66; }
[ -f "$AA_SYSROOT_MANIFEST" ] || { echo "error: AA_SYSROOT_MANIFEST is not a file: $AA_SYSROOT_MANIFEST" >&2; exit 66; }
[ -f "$AA_GOOGLETEST_ARCHIVE" ] || { echo "error: AA_GOOGLETEST_ARCHIVE is not a file: $AA_GOOGLETEST_ARCHIVE" >&2; exit 66; }
AA_SYSROOT_ROOTFS=$(realpath "$AA_SYSROOT_ROOTFS")
AA_SYSROOT_MANIFEST=$(realpath "$AA_SYSROOT_MANIFEST")
AA_GOOGLETEST_ARCHIVE=$(realpath "$AA_GOOGLETEST_ARCHIVE")

# Fail closed on the sandbox before touching anything else.
sandbox_backend=$(relay_sandbox_select "$sandbox_request") || exit 69
echo "relay: sandbox backend=$sandbox_backend" >&2

# Test hook (tests/ci/relay_sandbox_test.py): no network, no real sysroot validator or build.
test_mode=${AA_RELAY_TEST_MODE:-}

repo_root=$(git -C "$script_dir" rev-parse --show-toplevel)
scratch=$(realpath -m -- "${AA_RELAY_SCRATCH:-${TMPDIR:-/tmp}/arm64-cross-relay-$head_sha}")
base_tree=$scratch/base
pr_tree=$scratch/pr-tree
logs=$scratch/logs
empty_home=$scratch/empty-home
AA_SYSROOT_CACHE=$(realpath -m -- "${AA_SYSROOT_CACHE:-$scratch/build-binding}")

# Sandbox mount contract. Checked BEFORE anything is created: a writable path (tree, cache) that
# equals, contains or sits inside a read-only input (sysroot, manifest, archive, extra) would mask
# or expose it, so the relay refuses to run.
RELAY_SB_RW=("$pr_tree" "$AA_SYSROOT_CACHE")
RELAY_SB_RO=("$AA_SYSROOT_ROOTFS" "$AA_SYSROOT_MANIFEST" "$AA_GOOGLETEST_ARCHIVE")
# Observed-mode contract: the hash-validated payload IS the link sysroot (SysrootGate).
RELAY_SB_ENV=("AA_SYSROOT_ROOTFS=$AA_SYSROOT_ROOTFS" "AA_SYSROOT_MANIFEST=$AA_SYSROOT_MANIFEST"
    "AA_GOOGLETEST_ARCHIVE=$AA_GOOGLETEST_ARCHIVE" "AA_SYSROOT_CACHE=$AA_SYSROOT_CACHE"
    "AA_SYSROOT_PAYLOAD=$AA_SYSROOT_ROOTFS" "AA_SYSROOT_REQUIRE_OBSERVED=ON" "PKG_CONFIG_PATH=")
relay_sandbox_canonicalize || exit $?
mkdir -p "$logs" "$empty_home" "$pr_tree" "$AA_SYSROOT_CACHE"

if [ -n "$test_mode" ]; then
    base_sha=$(git -C "$repo_root" rev-parse "HEAD^{commit}")
else
    git -C "$repo_root" fetch origin main --quiet || echo "warning: git fetch origin main failed; relying on local objects" >&2
    git -C "$repo_root" fetch origin "$head_sha" --quiet 2>/dev/null || true
    base_sha=$(git -C "$repo_root" rev-parse "origin/main^{commit}") || {
        echo "error: origin/main not available; cannot establish the trusted base" >&2
        exit 66
    }
fi
git -C "$repo_root" cat-file -e "${head_sha}^{commit}" || {
    echo "error: commit $head_sha not present locally or on origin" >&2
    exit 66
}
if [ -z "$test_mode" ] && ! git -C "$repo_root" diff --quiet "$base_sha" -- tools/ci/arm64-cross-relay.sh tools/ci/relay-sandbox-lib.sh 2>/dev/null; then
    echo "warning: this relay script differs from origin/main; run it from a reviewed checkout of main, not the PR branch" >&2
fi

# The PR number binds the SHA and the author before any untrusted code is touched.
if [ -n "$pr_number" ]; then
    pr_info=$(gh pr view "$pr_number" --repo "$repo_slug" --json author,headRefOid         --jq '.author.login + " " + .headRefOid') || {
        echo "error: could not read PR #$pr_number through gh" >&2
        exit 69
    }
    pr_author=${pr_info%% *}
    pr_head=${pr_info##* }
    [ "$pr_head" = "$head_sha" ] || {
        echo "error: PR #$pr_number head is $pr_head, not $head_sha" >&2
        exit 65
    }
    author_ok=
    for allowed in "${allowed_authors[@]}"; do
        [ "$pr_author" = "$allowed" ] && author_ok=1
    done
    [ -n "$author_ok" ] || {
        echo "error: PR #$pr_number is authored by '$pr_author'; relay refuses non-owner authors unless --allow-author $pr_author is passed" >&2
        exit 65
    }
elif [ -z "$pr_number" ]; then
    echo "warning: no pr-number given; PR author and head-SHA binding are NOT verified" >&2
fi

git -C "$repo_root" worktree add --detach "$base_tree" "$base_sha" >/dev/null
cleanup() {
    local rc=$?
    # Whatever ends the relay (success, error, INT, TERM), the sandbox must be gone before the
    # tree is touched. If that cannot be confirmed, leave everything in place and say so.
    if ! relay_sandbox_reap_active; then
        echo "error: sandbox termination unconfirmed; leaving $pr_tree in place; investigate before reuse" >&2
        exit 70
    fi
    git -C "$repo_root" worktree remove --force "$base_tree" >/dev/null 2>&1 || true
    # The PR tree holds untrusted build output; logs stay under $scratch/logs.
    chmod -R u+rwX "$pr_tree" 2>/dev/null || true
    rm -rf "$pr_tree"
    exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
# Raw export (no .git, no attribute processing); see tools/ci/relay-export-tree.py.
python3 -I -B "$script_dir/relay-export-tree.py" "$repo_root" "$head_sha" "$pr_tree" || {
    echo "error: could not export $head_sha" >&2
    exit 65
}

step_names=()
step_rcs=()
step_shas=()
overall_rc=0
record_step() {
    local name=$1 rc=$2 log=$3
    step_names+=("$name")
    step_rcs+=("$rc")
    step_shas+=("$(sha256sum "$log" | cut -d' ' -f1)")
    [ "$rc" -eq 0 ] || overall_rc=1
}

# Trusted step: runs in the BASE checkout with a scrubbed environment, outside the sandbox.
# Only base-revision code runs here; the PR tree is never executed by it.
run_trusted_step() {
    local name=$1
    shift
    local log=$logs/$name.log rc
    echo "relay: trusted step=$name (base $base_sha) log=$log" >&2
    set +e
    (cd "$base_tree" && env -i PATH=/usr/bin:/bin HOME="$empty_home" GIT_CONFIG_NOSYSTEM=1 "$@") >"$log" 2>&1
    rc=$?
    set -e
    record_step "$name" "$rc" "$log"
}

# Sandboxed step: the PR tree is the working directory; nothing else of the owner's is visible.
run_sandbox_step() {
    local name=$1
    shift
    local log=$logs/$name.log rc
    echo "relay: sandboxed step=$name backend=$sandbox_backend log=$log" >&2
    set +e
    relay_sandbox_run "$sandbox_backend" "$pr_tree" "$@" >"$log" 2>&1
    rc=$?
    set -e
    if [ "$rc" -eq 71 ]; then
        # The sandbox could not be confirmed gone: untrusted code may still hold the mounts.
        # Do not hash logs, do not post any status; the EXIT trap keeps the tree in place.
        echo "error: relay: step=$name sandbox termination unconfirmed; no status posted" >&2
        exit 70
    fi
    echo "relay: sandbox exited step=$name rc=$rc" >&2
    record_step "$name" "$rc" "$log"
}

started_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
if [ -n "$test_mode" ]; then
    run_trusted_step 00-require-observed sh -c "${AA_RELAY_TEST_TRUSTED_CMD:-true}"
    run_sandbox_step 10-sandbox-probe sh -c "${AA_RELAY_TEST_SANDBOX_CMD:-true}"
else
    run_trusted_step 00-require-observed python3 -I -B tools/build/validate_sysroot.py \
        --manifest "$AA_SYSROOT_MANIFEST" --payload "$AA_SYSROOT_ROOTFS" --require-observed
    # Do not spend a sandbox on a tree whose sysroot did not validate.
    if [ "$overall_rc" -eq 0 ]; then
        run_sandbox_step 10-configure cmake --preset jetson-aarch64
    fi
    if [ "$overall_rc" -eq 0 ]; then
        run_sandbox_step 20-build cmake --build --preset jetson-aarch64
    fi
fi
completed_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
echo "relay: all sandboxed work finished; the status is posted from the trusted shell only now" >&2

if [ "$overall_rc" -eq 0 ]; then
    conclusion=success
    title="Observed-sysroot exact-SHA cross smoke PASS (sysroot jetson-r39.2.1)"
else
    conclusion=failure
    title="Observed-sysroot exact-SHA cross smoke FAIL (sysroot jetson-r39.2.1)"
fi

# Commit statuses have a short description limit, so include compact sha256 prefixes
# while the full per-log sha256 values remain recorded above in step_shas.
description="$(python3 -B - "$logs" "$conclusion" <<'PY'
import hashlib
import sys
from pathlib import Path

logs = Path(sys.argv[1])
outcome = sys.argv[2].upper()
hashes = []
for path in sorted(logs.glob('*.log')):
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    print(f'relay: log sha256 {path.name}={digest}', file=sys.stderr)
    hashes.append(digest[:8])
print(f'private exact-SHA ARM64 cross-smoke {outcome}; sysroot jetson-r39.2.1; log sha256 {hashes}')
PY
)"
if [ -n "$pr_number" ]; then
    target_url="https://github.com/zucch1/android-auto-jetson/pull/$pr_number"
else
    target_url="https://github.com/zucch1/android-auto-jetson/commit/$head_sha"
fi
gh api -X POST "repos/zucch1/android-auto-jetson/statuses/$head_sha" \
    -f "state=$conclusion" \
    -f context=ci/arm64-cross-build \
    -f "description=$description" \
    -f "target_url=$target_url"

echo "relay: observed smoke started=$started_at completed=$completed_at" >&2
echo "relay: posted ci/arm64-cross-build commit status for $head_sha state=$conclusion" >&2
exit "$overall_rc"
