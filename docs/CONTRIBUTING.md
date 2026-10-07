# Contribution and integration workflow

The repository is `zucch1/android-auto-jetson`; `main` is the integration branch.
Use short-lived `aa/<topic>` branches based on `main`, with one focused task per
pull request. Keep branch names short and descriptive. Do not introduce GitFlow,
CODEOWNERS, a merge queue, or mandatory Conventional Commits.

## Worktrees and review

- Give each agent its own branch and worktree. Only one agent writes a worktree;
  read-only review is allowed. Do not reset, overwrite, or clean another agent's
  dirty implementation or retained evidence.
- Coordinate file ownership before parallel edits. Integrate another worktree's
  changes only after reviewing its diff and validation evidence.
- Open a reviewed PR describing the scope, touched files, tests, assumptions,
  and outstanding gates. Review is required by this workflow even where the
  repository setting requires zero formal approvals.
- Merge only through an explicitly reviewed, manually authorized squash merge.
  No direct integration pushes, automatic merge, or bypass of pending checks.
  Do not commit, push, open a PR, or change repository rules without explicit
  authorization.

## CI contexts and private ARM64 companion

The planned merge gate consists of three exact, stable contexts:

| Context | Implementation in this change |
|---|---|
| `ci/host-build-test` | Ubuntu 24.04 host foundation configure/build/CTest including the task-5 target-safety suite (public job) |
| `ci/provenance-lint` | Source/notice and dependency-lock checks plus scratch mutation tests (public job) |
| `ci/arm64-cross-build` | Provisioned private companion workflow; not a public job (see below) |

`.github/workflows/ci.yml` runs the three independent nonmatrix public jobs on pull
requests targeting `main` or `aa/**`, and pushes to those branches. There are no
path filters that could silently omit a required context. Both jobs use only
`contents: read`, a full-commit-pinned checkout with credential persistence
disabled, and no artifact upload or cache actions.

The host build downloads the exact GoogleTest v1.15.2 archive specified by
`deps/manifest.json` outside configure, then verifies it against
`deps/manifest.lock` and the checker's independently anchored source identity
before staging. The release tag resolves to
`b514bdc898e2951020cbdca1304b75f5950d1f59`; its exact archive SHA-256 is
`7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926`.
A moved tag or altered archive must fail verification, not update the lock.
Checkout v4.2.2 was verified against the upstream tag as
`11bd71901bbe5b1630ceea73d27597364c9af683`. Review and independently verify
upstream identity before changing any action or source pin.

Task 5's final acceptance is PASS per the task-6 handoff. The private ARM64
cross-build is now provisioned as a companion workflow, not a public job. The
companion repository `zucch1/android-auto-jetson-ci-private` runs
`.github/workflows/arm64-cross-build.yml` (workflow_dispatch only, Ubuntu 24.04
hosted) against the reviewed public head SHA using a qualified private sysroot
bundle. Access is gated by a reviewed exact-SHA allowlist (`reviewed-shas.txt`):
only a public SHA that the owner has reviewed and explicitly listed may build;
unlisted, malformed, or injection-shaped SHAs are rejected before any install or
download. No raw sysroot, cache archive, or credential is committed to the public
tree or uploaded to public artifacts.

The public `ci/arm64-cross-build` status is not published automatically: no
narrow status GitHub App is provisioned, so the companion's `GITHUB_TOKEN`
cannot write public commit statuses. The initial relay is manual: the owner or
parent verifies the exact private workflow head, owner/actor, dispatch, approved
public SHA, run attempt, and completed build/test logs, then POSTs the
`ci/arm64-cross-build` status through the existing authorized local `gh`
channel. A passing local reproduction is not a substitute for that reviewed
live run. There is no fake public ARM64 job: the public workflow still exposes
only the three public jobs above, and no placeholder is registered as a required
check. Green public jobs are not full task-6 acceptance or authorization to
merge; a ruleset change to require these contexts is a separate, explicitly
authorized step. Hardware evidence is not replaced by host smoke tests.

## Local validation and privacy

Use [HOST_BUILD.md](HOST_BUILD.md) and [DEPENDENCIES.md](DEPENDENCIES.md) for the
offline host setup. With the verified archive supplied explicitly, reproduce
the host job from the repository root:

```bash
set -euo pipefail
mkdir -p /tmp/opencode
python3 -B tools/deps/check_manifest.py --lock deps/manifest.lock --archive "$AA_GOOGLETEST_ARCHIVE"
cmake --preset host-dev -DAA_GOOGLETEST_ARCHIVE="$AA_GOOGLETEST_ARCHIVE"
cmake --build --preset host-dev
ctest --preset host-dev --output-on-failure
```

The target-safety suites retain scratch fixtures under `/tmp/opencode` and are
exercised by `ctest` as `target_safety_*`; the subset that is not registered is
enumerated with a per-suite reason in `tests/target_safety/assert_coverage.py`.

Reproduce the independent provenance job without a sysroot or source download:

```bash
set -euo pipefail
python3 -B tools/deps/check_manifest.py --lock deps/manifest.lock
tools/provenance/check.sh
for case in baseline overlay content header extra generated license gpl inventory listing symlink missing mode; do
  python3 -B tests/provenance/scenarios.py "$PWD" "$case"
done
```

The mutation suite uses existing isolated, retained scratch fixtures; it never
modifies the vendored sources in the working tree. The host build compiles
GoogleTest/GoogleMock and the C++20 smoke target, not the full AASDK or a Jetson
runtime. Keep private `.local/` content, evidence, sysroots, logs, caches, phone
data, and credentials out of commits, PR attachments, and public CI uploads.
Review the exact staged file list before any authorized commit; do not use a
blanket add of a dirty worktree. Preserve historical evidence locally.
