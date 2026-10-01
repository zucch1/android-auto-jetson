# Dependency identities and offline workflow

`deps/manifest.json` records source identity, host package observations, and the
pending target binding. `deps/manifest.lock` binds the **raw manifest bytes**, the
authorized patch bytes, source commits, and GoogleTest archive checksum. The
stdlib-only checker independently fixes the authorized source identities; editing
both a manifest source SHA and its lock does not authorize a different source.
The lock is a reproducibility/integrity record, not a signed trust mechanism.

## Configure and verify

Obtain GoogleTest **before configuring**, through a separately reviewed download
or offline transfer. The required v1.15.2 archive is the codeload release archive
named in the manifest, with commit `b514bdc898e2951020cbdca1304b75f5950d1f59`
and SHA-256 `7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926`.
No configure or staging command downloads anything.

```sh
set -euo pipefail
python3 tools/deps/check_manifest.py --lock deps/manifest.lock --archive /path/to/googletest-v1.15.2.tar.gz
cmake -S . -B build/dependencies -G Ninja -DAA_GOOGLETEST_ARCHIVE=/path/to/googletest-v1.15.2.tar.gz
cmake --build build/dependencies --target gtest gmock
ctest --test-dir build/dependencies --output-on-failure
```

The archive is explicitly required and hashed before extraction. `stage.py` also
runs the original task-2 provenance checker. Only regular archive files are
materialized; absolute paths, parent traversal, links, and duplicate files are
rejected. A fresh stage is published at `build/<build-name>/deps/<stage-input-sha256>/`,
keyed by the raw lock plus the independently reconstructed identity receipt.
Reuse checks the complete file set, bytes, modes, dependency block, and identity
receipt against the anchored originals and the verified archive. Corrupt stages
fail closed and are preserved; use a fresh build directory to investigate/recover.
Audit scratch directories are retained. No existing stage is repaired in place.

The CMake configure executes the exact GoogleTest block extracted from the
patched effective AASDK CMake file. It exports
`FETCHCONTENT_SOURCE_DIR_GOOGLETEST` to the verified staged source, and the
effective block rejects missing staging before FetchContent can run. This uses
the installed CMake FetchContent source-directory override guarantee, rather than
relying on `FETCHCONTENT_FULLY_DISCONNECTED`. GoogleTest/GoogleMock targets are
real dependency targets; this configure does **not** compile the full AASDK.
The effective full AASDK source is available in the same stage for later tasks.

## Original and effective AASDK identity

Pristine `third_party/aasdk/` remains the complete 566-file original snapshot at
`9bf6adf933665dee26532201719fac14a047ccf1`; original upstream listing/blob anchors
remain unchanged. OAA stays a reference-only 250-file subset at
`61eab61c5f9968154ff1a80faa8c0a427b208479`. The manifest's AASDK/OAA archive
checksums describe complete `git archive --format=tar COMMIT` outputs, without
prefixes, not arbitrary recompressed GitHub archives or the retained OAA subset.

The only downstream patch is `deps/patches/aasdk-googletest.patch`. It preserves
upstream headers, replaces the moving GoogleTest reference with the immutable
commit, and adds the staging precondition. It is applied with GNU patch
`--batch --fuzz=0 --posix --output`, leaving original files untouched.

| Identity | Value |
|---|---|
| Original root CMake Git blob | `1b1eb749423a7d017a6917cf7199ee583436d34c` |
| Original root CMake SHA-256 | `57ca8a80811eb6aff8fb745b9e1cb1b69c7b28ba0151afc7b360e99448462c9b` |
| Patch SHA-256 | `d27b1c7099041042ad255c2cd16b7d61588b6b6aaa93b4c7d1b0c0ffb35b7091` |
| Effective root CMake Git blob | `954b321bbc80afe7314d31b829b861eadf25e599` |
| Effective root CMake SHA-256 | `f7f19407c890e75e68c723347ce63cdc80e3d107d869dd94067c103cf010c818` |

Each stage's `identity.json` records these identities and a deterministic digest
over all staged source-file hashes/modes. This is separate from the original
task-2 provenance inventory; regenerating that inventory cannot bless a patch.

## Host availability and target-binding seam

Installed host versions/architectures were observed with `dpkg-query`; apt-cache
candidates describe local package-index availability only. Missing packages have
no installed version. NVIDIA GStreamer is optional and unavailable on this host;
no JetPack version is taken as proof of a plugin's presence. Qt consumer inventory
covers Core/Gui/Widgets/OpenGL and Qml/Quick; GStreamer covers core and app/video/
audio/GL, runtime plugin families, software-H264 and Qt6 sink candidates.

Full AASDK compilation is currently blocked by missing `libprotobuf-dev`,
`libboost-dev`, `libboost-system-dev`, `libboost-log-dev`, `libssl-dev`, and
`libusb-1.0-0-dev`. Consumer development additionally needs
`libgstreamer1.0-dev` and `libgstreamer-plugins-base1.0-dev`. Nothing was installed
to turn this inventory green. Linux AASDK later uses system protobuf with
`SKIP_BUILD_PROTOBUF=ON` and `SKIP_BUILD_ABSL=ON`; protoc and linked protobuf must
match. The upstream "v30.0" message is not evidence of an installed dependency.
ABI qualification remains task 7.

The current target state is explicitly `pending-task5-extraction`, with no live
versions and no sysroot digest. Task 5 must introduce a versioned bound-target
schema/validator extension replacing this pending state with the actual immutable
`toolchains/jetson-sysroot-manifest.json` SHA-256. That validator must check every
target package name/version/architecture/source and every copied input hash before
cross-configure. It must reject both version drift and same-version content drift.
The current checker deliberately accepts only the current pending schema; task 5
must extend it with tests for the bound state, rather than keeping pending forever
or accepting a bare unverified target hash. Host versions are never target pins.

## Updating dependencies

1. Review the desired upstream commit/release and licenses. Reproduce original
   trees/listings/archive hashes independently; preserve original header notices.
   An AASDK/OAA pin change is also a task-2 provenance update, not just a lock edit.
2. Obtain and hash the exact GoogleTest archive outside configure. Verify the
   release-to-commit identity independently. Update the manifest and independently
   reviewed checker source constants together; branches are never acceptable pins.
3. Reproduce the patch against the new anchored original in a fresh audit directory
   with zero fuzz. Review the whole diff, original blob, patch SHA, and resulting
   blob/SHA. Update the checker patch authorization and this identity table.
4. Refresh host observations using `dpkg-query -W` and `apt-cache policy`, keeping
   missing/optional status distinct from installed versions. Do not promote an
   apt candidate to an installed pin. Task-5 target updates require a new immutable
   sysroot manifest/cache identity and later capability requalification.
5. Compute `sha256sum deps/manifest.json deps/patches/aasdk-googletest.patch` and
   explicitly update the lock after reviewing these inputs. There is no automatic
   "accept arbitrary hashes" mode. Re-run checker, ten manifest mutations, staging
   reuse/tamper/guard tests, and the unchanged provenance suite.
6. Run offline QA with a fresh build directory; retain the resulting command/trace
   evidence. Namespace isolation is preferred. Where `unshare -Urn` is denied,
   `tests/deps/offline.py` uses installed strace to deny socket creation/connect/
   sends in all descendants, proves denial with an AF_INET self-test, and rejects
   any connection/send or AF_INET socket attempt during successful configure.
   This fallback is syscall-denial evidence, **not** a loopback-only namespace claim.

```sh
set -euo pipefail
python3 -B tests/deps/offline.py "$PWD" /path/to/googletest-v1.15.2.tar.gz /path/to/task-3-dependencies.json
```

The evidence directory must exist. Test fixtures and QA builds are unique and
retained; no phone captures, phone identifiers, target contact, or target package
installation are involved in this dependency gate.
