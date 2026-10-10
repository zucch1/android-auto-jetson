# Offline sysroot artifact

The code under `tools/sysroot/` provides local materialization and validation of
an explicitly supplied snapshot. It does not acquire files from a Jetson, qualify
a target, or make the ARM64 cross-build ready. The artifact format and CLI below
describe the implemented offline boundary.

Current task-6 handoff: task 5's final acceptance is PASS. The prerequisite
failure and outstanding-gate notes below are historical, not a current block
on the independent host CI jobs. They remain preserved and are not evidence
of present target reachability. Private ARM64 CI runs as a companion workflow
outside this repository; no ARM64 CI job is implemented here. See
[CONTRIBUTING.md](CONTRIBUTING.md) for the public contexts and the companion
ARM64 integration context. This status update performs no target
access and grants no live-acquisition authority.

## CLI

Run from the repository root with Python 3.12 or later. The destination argument
is an existing, trusted parent directory; materialization creates a child named
for the manifest version. For example, with local paths supplied by the operator:

```sh
python3 -B tools/sysroot/cli.py materialize \
  --snapshot ./input-snapshot \
  --metadata ./input-metadata.json \
  --destination ./private-sysroots
```

The three commands are:

```text
python3 -B tools/sysroot/cli.py materialize --snapshot SNAPSHOT --metadata METADATA --destination PARENT
python3 -B tools/sysroot/cli.py validate --artifact PARENT/VERSION
python3 -B tools/sysroot/cli.py require-observed --artifact PARENT/VERSION
```

`materialize` reads only the given local snapshot and metadata. The snapshot is
the source filesystem tree and the metadata is an `aa-sysroot-1` JSON manifest.
`validate` checks artifact integrity. `require-observed` performs the same
integrity validation and additionally rejects manifests whose declared
`provenance` is `fixture`; it accepts `observed` as a declaration only, not as
verified provenance. Neither validation command authenticates the artifact or
qualifies it for a target or cross-build.

The compatibility wrapper has "extract" in its filename, but is offline-only
materialization and invokes no target or network commands:

```sh
bash tools/sysroot/extract-jetson-sysroot.sh \
  --snapshot ./input-snapshot \
  --metadata ./input-metadata.json \
  --destination ./private-sysroots
```

On success, each command prints one JSON object to stdout containing `artifact`,
`payload_root`, `manifest_sha256`, `reused`, `provenance`, and `capability`.
Handled operational failures use a JSON object on stderr (`code` and `field`,
or `code` and `errno`) and a nonzero exit status. Argument parsing errors use
argparse's text diagnostics instead. Relevant codes include `MALFORMED_METADATA`,
`UNSAFE_PATH`, `DUPLICATE`, `PACKAGE_OWNER`, `SYMLINK_CLOSURE`,
`INVENTORY_MISMATCH`, `HASH_MISMATCH`, `ARTIFACT_MISMATCH`,
`NEW_VERSION_REQUIRED`, `FIXTURE_PROVENANCE`, and `LOCAL_IO`.

## Artifact layout and manifest

For manifest version `VERSION`, the concrete artifact is:

```text
PARENT/VERSION/
|-- manifest.json
`-- rootfs/
    `-- <staged paths from the manifest>
```

The version is a single safe path component. The artifact contains exactly
`manifest.json` and `rootfs`. The manifest uses schema `aa-sysroot-1`, includes
the version, `fixture` or `observed` provenance, nullable kernel/L4T identity values,
package records, and file records, and always has `capability: "pending"`.
Package records carry Debian package name, version, architecture (`arm64` or
`all`), and source; `all` packages require an explicit justification. Each file
record binds an absolute original input path, a rootfs-relative staged path,
the owning declared package, and its kind and digest. Debian `arm64` corresponds
to GNU/CMake `aarch64` for the ARM64 target ISA. Debian `all` means
architecture-independent, not an ARM64 target ISA.

Manifest JSON is canonicalized as UTF-8 JSON with sorted keys, compact separators,
and a trailing newline. `manifest_sha256` is SHA-256 of those canonical manifest
bytes. It is an integrity identifier, not a signature or authenticity proof;
consumers must independently bind the returned digest where required. The
artifact does not contain a separate digest file.

## Validation and filesystem safety

The parser rejects duplicate JSON keys, unknown or missing fields, malformed
metadata, duplicate package/input/staged paths, unsafe paths, and file records
whose package owner is absent. Paths cannot be absolute in the staged tree or
contain traversal components or backslashes. Symlinks must use relative link
text. Their SHA-256 is over the UTF-8 link text, and closure checking requires
every symlink chain to terminate at a declared regular file without cycles,
escape, or changed referents after relocation into `rootfs`.

For regular files, SHA-256 is over raw file bytes. Materialization and validation
check exact source/artifact leaf and directory inventories and file hashes.
Symlinks are checked by their recorded link text without following them. Filesystem
access uses descriptor-relative, no-follow opens for path components and regular
files. The staged artifact requires regular files and `manifest.json` to have
mode `0444`, and directories (including the artifact and `rootfs`) to have mode
`0555`. These modes deter accidental writes; they are not owner-proof immutability
and an owner can deliberately change permissions.

## Version publication and reuse

Materialization validates the explicit source against its manifest, copies into
a temporary directory under the destination parent, revalidates the source,
sets the modes above, validates the staged payload, then publishes atomically using
Linux `renameat2(RENAME_NOREPLACE)`. The destination parent is locked for
cooperating local publishers. Publication fails closed if no-replace rename is
unavailable, and an existing version is never repaired or overwritten.

If `PARENT/VERSION` already exists, it must itself validate and its manifest must
match the requested manifest; then it is reused after the source is revalidated.
An invalid existing artifact returns `ARTIFACT_MISMATCH`. A valid existing
artifact with a differing requested manifest returns `NEW_VERSION_REQUIRED`;
neither is updated in place. Use a new version for changed content or metadata.
This immutability is enforced by publication policy and accidental-write-protection
modes, not by a privileged or cryptographic
owner-proof mechanism.

## Provenance and qualification limits

`observed` is a value supplied in metadata. `require-observed` only rejects the
`fixture` label; it does not prove that the files were observed on a Jetson,
verify package origins, establish authenticity, or qualify target compatibility.
The manifest's capability remains pending in all cases. Do not interpret a
successful CLI result or an `observed` label as task-5 acceptance.

The production dependency binding carries the immutable observed sysroot
manifest digest and the authoritative toolchain package records; the required
receiver components (protobuf, Boost, OpenSSL, libusb, GStreamer, Qt6) have no
target package observations and remain explicitly unbound. There is no
qualified ARM64 CMake preset,
validated complete cross-build input closure, cross-build result, or immutable
private-cache qualification established by this offline CLI. Synthetic/local
validation is not evidence of working ARM64 binaries or a target integration
test.

The existing task-5 prerequisite guard is separate from this materializer. It
supports only a bounded, read-only target observation claim when a complete
guarded window and watch closure succeed; it does not extract or transfer a
sysroot. Live acquisition requires separate review of guarded target reads and
fresh authorization; this offline workflow performs no SSH, target access,
package installation, or remediation. The supervisor that temporarily changes
the target inotify watch ceiling is a separate operation, is not read-only, and
has best-effort restoration limits.

Historical prerequisite attempts failed to establish complete guarded target
observation. Current reachability remains unproven by this documentation work;
current target results belong in evidence, not this product document. This work
establishes no target observations or authorization for live acquisition.
At the time of the historical offline-only checkpoint, task 5 remained gated
on its outstanding guarded acquisition,
bound manifest/dependency update,
cross-build integration and qualification, and private-cache qualification;
task 6 remained gated on task-5 acceptance. No test execution or target result
was claimed by that checkpoint; its evidence is unchanged.
