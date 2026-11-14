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

The public CI workflow (`.github/workflows/ci.yml`) executes independent, nonmatrix public jobs. The workflow file is the authority for their names; today they are:

| Context | Implementation in `.github/workflows/ci.yml` (or owner relay) |
|---|---|
| `ci/host-build-test` | Ubuntu 24.04 host build and test suite, including AASDK ABI, IPC, protocol, transport, audio, display, wired E2E, recovery, target-safety, and binary hardening suites (public required job) |
| `ci/decode-overlay-cross` | Synthetic cross fixture dependencies, AArch64 ELF verification, and external overlay provenance check (public job) |
| `ci/base-validators` | Runs `tools/deps/check_manifest.py`, `tools/provenance/check.py`, and `tools/tls/scan.py` from `main` against the PR tree in a separate fresh-runner scrubbed environment; gates `ci/provenance-lint` (public job; optionally required in the ruleset) |
| `ci/provenance-lint` | Requires `ci/base-validators` to have succeeded, then runs in-tree dependency lock checks (`check_manifest.py`), source/notice verification (`tools/provenance/check.sh`), scratch mutation tests, and protobuf audit verification (`protobuf_audit.py --check`, `audit_scenarios.py`) (public required job) |
| `ci/arm64-cross-fixture` | Fixture-sysroot cross-build (`make_sysroot`, `AA_SYSROOT_REQUIRE_OBSERVED=OFF`, `cmake --preset jetson-aarch64`) building targets including `aa_ipc_control` and `aa_ipc_dbus`, verifying `assert_dbus_coverage.py --cross` and `build_hardening` (public **required** job, explicitly **fixture-not-target**) |
| `ci/arm64-cross-build` | **Optional target evidence** (not a workflow job and not a required check): commit status posted only by the owner-run sandboxed relay `tools/ci/arm64-cross-relay.sh` after an observed-sysroot cross-build (`AA_SYSROOT_REQUIRE_OBSERVED=ON` against `sysroot jetson-r39.2.1`) |

`.github/workflows/ci.yml` runs the five public jobs on pull requests targeting `main` or `aa/**`, on pushes to `main`, and on manual `workflow_dispatch`. Pushes to `aa/**` branches start no separate run: the PR run already reports every required context on the head commit, and a newer push to the same PR cancels the superseded PR run. There are no workflow path filters that could silently omit a required context.

Inside `ci/host-build-test`, PR runs skip only the 600 s `ipc-bench-happy-600` throughput gate, and only when `tools/ci/ipc_bench_gate.py` shows that the PR leaves the benchmark's sources, every ninja command that builds `aa-ipc-bench` and its test registration identical to the PR base (any difference or error runs it; `ci_ipc_bench_gate` tests the classification). A PR that edits `.github/workflows/ci.yml` always runs it, since runner-image or provisioning changes do not show up in that comparison. Every other test (including `input_sensors_memory` under Valgrind memcheck) runs on every PR, and the throughput gate runs on every push to `main` and on dispatch, so a throughput regression that does not come from the PR's own changes to the benchmark build (for example runner image or toolchain drift) is caught after merge, not before. The jobs use only `contents: read`, a full-commit-pinned checkout with credential persistence disabled, and no artifact upload or cache actions.

The host build downloads the exact GoogleTest v1.15.2 archive specified by `deps/manifest.json` outside configure, then verifies it against `deps/manifest.lock` and the checker's independently anchored source identity before staging. The release tag resolves to `b514bdc898e2951020cbdca1304b75f5950d1f59`; its exact archive SHA-256 is `7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926`. A moved tag or altered archive must fail verification, not update the lock. Checkout v4.2.2 was verified against the upstream tag as `11bd71901bbe5b1630ceea73d27597364c9af683`. Review and independently verify upstream identity before changing any action or source pin.

### Validators run from the default branch on every run

On every run (`pull_request`, `push` and `workflow_dispatch` alike) the separate job
`ci/base-validators` runs
`tools/deps/check_manifest.py` (against `deps/manifest.lock`),
`tools/provenance/check.py` (which imports `tools/provenance/inventory.py`) and
`tools/tls/scan.py` from the repository **default branch** (`main`, never the PR's base ref: a stacked `aa/X`
base is author-pushable) against the tree under test, so one PR
cannot change a check and the data it checks together and stay green. It is a
**separate job on a fresh runner**: no PR code executes before it (so a PR step cannot
poison later "trusted" steps through `$GITHUB_ENV`/`BASH_ENV`, `$GITHUB_PATH` or a shell
function). It checks out `main` (validator code) and the tree under test (data only:
only ever a path argument, nothing from it is executed or sourced) and runs the validators
with `/usr/bin/python3 -B -s -E` from a scrubbed `env -i`.

Which job produces which required status: the required context `ci/provenance-lint` keeps
its name and its existing in-tree steps (including the protobuf audit and scenario checks),
and now has `needs: base-validators` plus a first
step that fails unless `ci/base-validators` **succeeded**, for every event type (the job runs under
`!cancelled()`, because a *skipped* required job would otherwise count as passing; `skipped` is
not accepted, so `gh workflow run ci.yml --ref <pr-branch>` cannot produce a green
`ci/provenance-lint` without the trusted validators). `pull_request` runs on `opened`,
`synchronize` and `reopened` only: `edited` would cancel and restart the whole run (including the
600 s ipc bench when it applies) on every title or body edit, and since the validators always come
from `main`, a retarget no longer changes what they check. **After retargeting a PR, close and
reopen it, or push a commit, to rerun CI.** Do not replace this with job-level `if:` skipping: a
skipped required job counts as passing. Optionally add `ci/base-validators` to the ruleset's
required contexts after its first run.

If a PR legitimately changes one of these validators or their pinned constants, the base run
fails by design: land the validator change on its own first (the owner reviews it explicitly),
then rebase the data change on it. That order does not work when a validator's pinned constant and
the data it checks must change together (`check_manifest.py` + `deps/manifest.lock`): make the
change backward compatible (the old validator must accept the new data, or the new validator the
old data) so the two can land in either order, or accept a red `main` between two merges, or use
an admin bypass for that merge.

**Residuals.** (a) Any file under `.github/workflows/` comes from the PR, not just `ci.yml`: a PR
can edit or delete these jobs, and can add a *new* workflow with a job named `ci/provenance-lint`
(or `ci/base-validators` or `ci/arm64-cross-fixture`). Any change under `.github/workflows/` needs
explicit owner review (enforcing it needs `pull_request_target`/ruleset work, not done here).
(b) The `workflow_dispatch` closure (a dispatch on an old PR branch cannot turn the gate green)
relied on the ruleset's **strict "require branches to be up to date before merging"** setting.
**Strict is now turned OFF** to make merges faster, which reopens that hole: a branch that
predates the `ci/base-validators` job carries the old gate, so a manual `workflow_dispatch` on
it can produce a green `ci/provenance-lint` without running the trusted validators. As the
compensating practice, the orchestrator merges only branches whose latest CI run used the
current workflow, and checks `main`'s runs after each batch of merges.
`tests/ci/ci_workflow_trust_test.py` (ctest `ci_workflow_trust`) guards the workflow structure.

### ARM64 fixture check and optional observed-sysroot relay

No required CI check depends on the owner. The required context for the ARM64 cross-build is
`ci/arm64-cross-fixture`, posted by GitHub Actions. It executes against a synthetic fixture
sysroot generated in `RUNNER_TEMP` (`tests/media/decode_cross_fixture.py`,
`AA_SYSROOT_REQUIRE_OBSERVED=OFF`), builds `cmake --preset jetson-aarch64` plus `aa_ipc_control`
and `aa_ipc_dbus`, and verifies toolchain invocation, compilation, cross inventory
(`assert_dbus_coverage.py --cross`), and executable hardening (`build_hardening`). This public
cross-build is explicitly **fixture-not-target**: it is NOT physical target qualification.

`ci/arm64-cross-build`, posted only by the owner-run sandboxed relay
(`tools/ci/arm64-cross-relay.sh`), is **optional target evidence**, not a required CI check.
What this gives up: `main` can accept a change that was only cross-built against the synthetic
fixture sysroot, never against the real Jetson sysroot (`sysroot jetson-r39.2.1`). The relay
remains the way to obtain that observed-sysroot proof (`AA_SYSROOT_REQUIRE_OBSERVED=ON`) for a
reviewed exact commit SHA. Acceptance of observed-sysroot evidence is bound to the exact SHA:
after any squash merge, rebase, or sync, the head SHA changes and the smoke must be re-run and
re-posted for the new SHA — never relay a stale SHA. Passing a local reproduction or synthetic
fixture run is not a substitute for that reviewed exact-SHA run; green public jobs (including
`ci/arm64-cross-fixture`) are not target acceptance or authorization to skip hardware evidence.

### Owner-run sandboxed relay

Where the observed sysroot is on the owner machine, `tools/ci/arm64-cross-relay.sh`
produces and posts the optional `ci/arm64-cross-build` commit status directly (it replaces
running the PR tree's own build with the owner's credentials). The observed-sysroot ARM64
cross-build is relayed at a reviewed, exact head SHA when target-sysroot proof is desired. The
relay treats the PR tree as untrusted code:

- Run it from your own checkout of `main`, never from the PR branch. It fetches
  `origin/main` into a base worktree and runs `tools/build/validate_sysroot.py` from
  there (scrubbed environment), so the PR cannot change the validator that judges it.
- The PR head is exported by `tools/ci/relay-export-tree.py` (raw blobs from
  `git ls-tree`/`git cat-file`, **not** `git archive`: a PR's `.gitattributes` must not
  be able to select a filter or `export-subst` that runs on your machine; symlinks are
  written but never followed, gitlinks are skipped, `.git`/`..` paths are rejected) into
  a scratch directory and built
  (`cmake --preset jetson-aarch64`, `cmake --build --preset jetson-aarch64`) only
  inside a sandbox with no `$HOME`, no `gh` auth, no SSH agent, no network, no inherited
  file descriptors (every fd above 2 is closed before the sandbox starts; stdin is
  `/dev/null`), the sysroot, manifest and GoogleTest archive mounted read-only, and a
  complete scrubbed environment (`env -i`-style: fixed `PATH`, `GIT_CONFIG_NOSYSTEM=1`,
  empty `HOME`). All writable and read-only paths are canonicalised first and the relay
  refuses to run if a writable path (`AA_SYSROOT_CACHE`, the scratch tree) equals,
  contains or sits inside a read-only input such as the sysroot, equals or contains your
  `$HOME` or the repo checkout, or overlaps the repo's git dirs or the relay's own logs,
  base checkout and empty-HOME directories. The repo checkout is fully protected: a
  writable path may not equal it, contain it or sit inside it (so `AA_SYSROOT_CACHE=<repo>/build`
  or `<repo>/tools/ci` is refused: the sandbox could otherwise rewrite the host `build.ninja` or
  the running relay script). `$HOME` itself may not be equalled or contained, but a dedicated
  directory inside it is fine.
- `AA_SYSROOT_CACHE` and an explicit `AA_RELAY_SCRATCH` must be nonexistent, empty, or hold
  the `.aa-relay-dedicated` marker file the relay creates on first use; a non-empty directory
  without it (`~/.ssh`, `~/.config`, a project) is refused with exit 65. An existing
  `AA_RELAY_SCRATCH` must also be owned by you and not group/world-writable.
- Scratch: by default a fresh `mktemp -d` (mode 0700) under `$TMPDIR`, else `/var/tmp` (never
  `$XDG_RUNTIME_DIR`: a RAM tmpfs is too small for the cross build). The cleanup trap is
  installed before anything is created, so a refused or failed run (fetch, missing commit, PR
  binding, author check) leaves nothing behind. On exit the relay removes everything except
  `logs/` (the status carries their sha256s), and prints where the scratch is; a default
  scratch with no logs disappears entirely.
- Use a fresh, dedicated `AA_SYSROOT_CACHE` for each run (or leave it unset: it is then
  `<scratch>/build-binding` and removed at exit): hardlink or bind-mount aliases into the
  cache, and cache poisoning that carries over between PRs, are not defended against.
- `SIGKILL` of the relay (or a second signal while it is cleaning up) can leave a
  container behind; check `docker ps` / `podman ps` and remove it by hand.
- Containers (podman/docker) get a random name. After every step, and on timeout, error,
  INT, TERM or exit, the relay `kill`s and `rm -f`s the container and verifies it is gone
  (the daemon must answer and list no such container) before it hashes logs or posts a
  status. If that cannot be confirmed it posts nothing, leaves the scratch tree in place
  and exits 70: investigate before reusing the machine state.
- Only after the sandbox has exited does the trusted script post the optional
  `ci/arm64-cross-build` commit status for that exact SHA with `gh`, using your own
  auth, outside the sandbox. Re-run for the new SHA after every push or sync.
- With a PR number the relay first checks that the SHA is that PR's head and that the
  PR author is the repository owner; pass `--allow-author LOGIN` to relay another
  author's PR you have reviewed. Without a PR number that check is skipped (with a
  warning). The relay only proves the tree builds against the observed sysroot; it
  does not make an unreviewed PR safe, so read the diff first.

```bash
export AA_SYSROOT_ROOTFS=... AA_SYSROOT_MANIFEST=... AA_GOOGLETEST_ARCHIVE=...
tools/ci/arm64-cross-relay.sh [--sandbox auto|podman|bwrap|docker] [--allow-author LOGIN] <head-sha> [pr-number]
```

The initial executable must come from an **independently reviewed full commit SHA**.
Never run a PR checkout's relay with owner credentials. For the #92 bootstrap (main
still has the unsafe relay), the owner must review the exact raw bytes of
`arm64-cross-relay.sh`, `relay-sandbox-lib.sh` and `relay-export-tree.py`, then install
that pinned set in a separate owner-only repository. Choose the SHA explicitly after
review; do not derive it from a mutable PR ref at execution time:

```bash
# Set REVIEWED_SHA to the full 40-character SHA you independently reviewed.
: "${REVIEWED_SHA:?set the independently reviewed SHA}"
install_dir=$(mktemp -d "${TMPDIR:-/var/tmp}/aa-trusted-relay.XXXXXXXXXX")
git -C "$install_dir" init
git -C "$install_dir" remote add origin https://github.com/zucch1/android-auto-jetson.git
git -C "$install_dir" fetch origin main "$REVIEWED_SHA"
mkdir -p "$install_dir/tools/ci"
for file in arm64-cross-relay.sh relay-sandbox-lib.sh relay-export-tree.py; do
  git --no-replace-objects -C "$install_dir" cat-file blob "$REVIEWED_SHA:tools/ci/$file" > "$install_dir/tools/ci/$file" || exit 1
done
AA_RELAY_TRUSTED_REV="$REVIEWED_SHA" bash "$install_dir/tools/ci/arm64-cross-relay.sh" <head-sha> <pr-number>
```

The pin is an owner trust decision, not evidence that a commit was reviewed. It
covers the exporter as well as the executable and sourced library. After installation
on main, omit `AA_RELAY_TRUSTED_REV`: the relay requires a successful refresh of
`origin/main` and exact raw-byte matches of all three local files **before sourcing
anything**. Divergence, missing files, symlinks or an invalid pin stop the run; there is
no warning-only fallback. The entrypoint cannot authenticate itself if it was replaced
by malicious code: the separate, reviewed installation is essential. Use a trusted
shell/environment (no PR-provided `BASH_ENV`, PATH tools or Git configuration).

`AA_RELAY_TEST_MODE` runs probes instead of sysroot validation/builds and exits before
all status-posting code, on success and failure. It can never publish a production
`ci/arm64-cross-build` status. Tests install their own isolated, pinned fixture;
that fixture is never used for a real attestation.

Sandbox backend: `auto` picks rootless `podman` (needs `AA_RELAY_IMAGE`, a local image
with cmake, ninja, aarch64 g++ and python3), then `bwrap` (bubblewrap over the host's
read-only `/usr`; expose a toolchain installed elsewhere with `AA_RELAY_EXTRA_RO=/path:/path2`),
then `docker` (also needs `AA_RELAY_IMAGE`; its daemon is root-equivalent, so it is last).
If none is usable the relay stops; there is no unsandboxed mode.
`tests/ci/relay_sandbox_test.py` (ctest `ci_relay_sandbox`) proves the isolation (including
leaked fds, the original credential path, mount overlap/order and, when docker plus a local
image exist, container termination), fail-closed bootstrap checks, and suppression of all
test-mode statuses; a separate production-mode failure checks the status channel;
`tests/ci/relay_export_test.py` (`ci_relay_export`) proves the filter-free export. Both skip or
shrink with a reason where no backend works.

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

Reproduce the independent provenance job (`ci/base-validators` + `ci/provenance-lint`)
without a sysroot or source download:

```bash
set -euo pipefail
python3 -B tools/deps/check_manifest.py --lock deps/manifest.lock
tools/provenance/check.sh
python3 -B tools/tls/scan.py "$PWD"
for case in baseline overlay content header extra generated license gpl inventory listing symlink missing mode; do
  python3 -B tests/provenance/scenarios.py "$PWD" "$case"
done
python3 -B tools/protocol/protobuf_audit.py --check
for case in baseline drift-inventory report-drift missing-file tampered-proto unpinned-file listing-tampered repin-comment field-number-changed field-type-changed enum-value-removed new-unmatched-message vanished-difference unparsed-construct finding-dangling-diff uncovered-by-removal duplicate-id missing-field invalid-disposition status-mismatch unexplained-deletion nonlegacy-deletion legacy-altered citation-broken absence-stale crosslink-dangling orphan-finding mapping-dangling triage-missing-diff triage-dangling-diff triage-invalid-disposition triage-missing-metadata sentinel-false-match genuine-rename-still-matches renamed-enums-path-only zero-only-enum-false-match message-path-shape-boundary; do
  python3 -B tests/protocol/audit_scenarios.py "$PWD" "$case"
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
