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

## CI contexts and ARM64 cross-build

The public CI workflow (`.github/workflows/ci.yml`) executes four independent, nonmatrix public jobs:

| Context | Implementation in `.github/workflows/ci.yml` |
|---|---|
| `ci/host-build-test` | Ubuntu 24.04 host build and test suite, including AASDK ABI, IPC, protocol, transport, audio, display, wired E2E, recovery, and target-safety suites (public job) |
| `ci/decode-overlay-cross` | Synthetic cross fixture dependencies, AArch64 ELF verification, and external overlay provenance check (public job) |
| `ci/provenance-lint` | Source/notice verification, dependency lock checks (`check_manifest.py`), and scratch mutation tests (public job) |
| `ci/arm64-cross-build` | Fixture-sysroot cross-build (`make_sysroot`, `AA_SYSROOT_REQUIRE_OBSERVED=OFF`, `cmake --preset jetson-aarch64`) building targets including `aa_ipc_control`, `aa_ipc_dbus` (public job, explicitly **fixture-not-target**) |

`.github/workflows/ci.yml` runs these four public jobs on pull requests targeting `main` or `aa/**`, on pushes to `main`, and on manual `workflow_dispatch`. Pushes to `aa/**` branches start no separate run: the PR run already reports every required context on the head commit, and a newer push to the same PR cancels the superseded PR run. There are no workflow path filters that could silently omit a required context.

Inside `ci/host-build-test`, PR runs skip only the 600 s `ipc-bench-happy-600` throughput gate, and only when `tools/ci/ipc_bench_gate.py` shows that the PR leaves the benchmark's sources, every ninja command that builds `aa-ipc-bench` and its test registration identical to the PR base (any difference or error runs it; `ci_ipc_bench_gate` tests the classification). A PR that edits `.github/workflows/ci.yml` always runs it, since runner-image or provisioning changes do not show up in that comparison. Every other test runs on every PR, and the gate runs on every push to `main` and on dispatch, so a throughput regression that does not come from the PR's own changes to the benchmark build (for example runner image or toolchain drift) is caught after merge, not before. The jobs use only `contents: read`, a full-commit-pinned checkout with credential persistence disabled, and no artifact upload or cache actions.

The host build downloads the exact GoogleTest v1.15.2 archive specified by `deps/manifest.json` outside configure, then verifies it against `deps/manifest.lock` and the checker's independently anchored source identity before staging. The release tag resolves to `b514bdc898e2951020cbdca1304b75f5950d1f59`; its exact archive SHA-256 is `7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926`. A moved tag or altered archive must fail verification, not update the lock. Checkout v4.2.2 was verified against the upstream tag as `11bd71901bbe5b1630ceea73d27597364c9af683`. Review and independently verify upstream identity before changing any action or source pin.

The public `ci/arm64-cross-build` job executes against a synthetic fixture sysroot generated in `RUNNER_TEMP` (`tests/media/decode_cross_fixture.py`, `AA_SYSROOT_REQUIRE_OBSERVED=OFF`). It verifies toolchain invocation, compilation, and cross inventory (`assert_dbus_coverage.py --cross`). This public cross-build is explicitly **fixture-not-target**: it is NOT physical target qualification.

Target qualification evidence requires the separate observed-sysroot cross-smoke (`sysroot jetson-r39.2.1`), relayed by the maintainer via `tools/ci/arm64-cross-relay.sh:5-18`. The relay executes the observed-sysroot smoke (`AA_SYSROOT_REQUIRE_OBSERVED=ON`) against the exact commit SHA and posts the `ci/arm64-cross-build` commit status through the maintainer's local `gh` channel. Acceptance is strictly bound to the reviewed exact-head (exact-SHA) commit: after any squash merge, rebase, or sync, the head commit SHA changes and the smoke must be re-run and re-posted for the new exact SHA — never relay a stale SHA. Passing a local reproduction or synthetic fixture run is not a substitute for that reviewed exact-SHA run. Green public jobs are not full target acceptance or authorization to merge; hardware evidence is not replaced by host smoke tests or synthetic cross-builds.

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
modifies the vendored sources in the working tree. The native presets (`host-dev`,
`host-release`, `host-asan`) build upstream AASDK (`cmake/AasdkAbi.cmake`), the
receiver, and test targets; this establishes host and ABI compatibility, but
does not establish Jetson qualification. Keep private `.local/` content, evidence,
sysroots, logs, caches, phone data, and credentials out of commits, PR attachments,
and public CI uploads.
Review the exact staged file list before any authorized commit; do not use a
blanket add of a dirty worktree. Preserve historical evidence locally.
