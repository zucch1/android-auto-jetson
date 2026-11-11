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
| `ci/arm64-cross-build` | Posted only by the owner-run sandboxed relay `tools/ci/arm64-cross-relay.sh` (see below); the public Actions job is `ci/arm64-cross-fixture` and cannot satisfy it |

`.github/workflows/ci.yml` runs the independent nonmatrix public jobs (the two public required contexts plus `ci/decode-overlay-cross` and the fixture-only `ci/arm64-cross-fixture`) on pull
requests targeting `main` or `aa/**`, on pushes to `main`, and on manual
`workflow_dispatch`. Pushes to `aa/**` branches start no separate run: the
PR run already reports every required context on the head commit, and a newer push
to the same PR cancels the superseded PR run. There are no workflow path filters
that could silently omit a required context. Inside `ci/host-build-test`, PR runs
skip only the 600 s `ipc-bench-happy-600` throughput gate, and only when
`tools/ci/ipc_bench_gate.py` shows that the PR leaves the benchmark's sources, every
ninja command that builds `aa-ipc-bench` and its test registration identical to the
PR base (any difference or error runs it; `ci_ipc_bench_gate` tests the
classification). A PR that edits `.github/workflows/ci.yml` always runs it, since
runner-image or provisioning changes do not show up in that comparison. Every other test runs on every PR, and the gate runs on every
push to `main` and on dispatch, so a throughput regression that does not
come from the PR's own changes to the benchmark build (for example runner image or
toolchain drift) is caught after merge, not before. Both jobs use only
`contents: read`, a full-commit-pinned checkout with credential persistence
disabled, and no artifact upload or cache actions.

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
its name and its existing in-tree steps, and now has `needs: base-validators` plus a first
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
then rebase the data change on it. That does not work when a validator's pinned constant and
the data it checks must change together (`check_manifest.py` + `deps/manifest.lock`): make the
change backward compatible (the old validator must accept the new data, or the new validator the
old data) so the two can land in either order, or accept a red `main` between two merges, or use
an admin bypass for that merge.

**Residuals.** (a) Any file under `.github/workflows/` comes from the PR, not just `ci.yml`: a PR
can edit or delete these jobs, and can add a *new* workflow with a job named `ci/provenance-lint`
(or `ci/base-validators`). Any change under `.github/workflows/` needs explicit owner review
(enforcing it needs `pull_request_target`/ruleset work, not done here). (b) The
`workflow_dispatch` closure (a dispatch on an old PR branch cannot turn the gate green) relies on
the ruleset's **strict "require branches to be up to date before merging"** setting: an old PR
branch carries the old gate until `main` is merged into it. Turning strict off reopens that hole.
`tests/ci/ci_workflow_trust_test.py` (ctest `ci_workflow_trust`) guards this structure.

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
live run. The public workflow's fixture job is named `ci/arm64-cross-fixture`, never
`ci/arm64-cross-build`, so only an owner-posted status can satisfy the required
context. Green public jobs are not full task-6 acceptance or authorization to
merge. Hardware evidence is not replaced by host smoke tests.

### Owner-run sandboxed relay

Where the observed sysroot is on the owner machine, `tools/ci/arm64-cross-relay.sh`
produces and posts the same status directly (it replaces running the PR tree's own
build with the owner's credentials). The observed-sysroot ARM64 cross-build is relayed
at a reviewed, exact head SHA, just before the squash merge. The relay treats the
PR tree as untrusted code:

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
- Only after the sandbox has exited does the trusted script post the
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

Sandbox backend: `auto` picks rootless `podman` (needs `AA_RELAY_IMAGE`, a local image
with cmake, ninja, aarch64 g++ and python3), then `bwrap` (bubblewrap over the host's
read-only `/usr`; expose a toolchain installed elsewhere with `AA_RELAY_EXTRA_RO=/path:/path2`),
then `docker` (also needs `AA_RELAY_IMAGE`; its daemon is root-equivalent, so it is last).
If none is usable the relay stops; there is no unsandboxed mode.
`tests/ci/relay_sandbox_test.py` (ctest `ci_relay_sandbox`) proves the isolation (including
leaked fds, the original credential path, mount overlap/order and, when docker plus a local
image exist, container termination) and the post-after-sandbox ordering with a probe command;
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
