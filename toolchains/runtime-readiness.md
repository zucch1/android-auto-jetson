# Jetson cross-runtime readiness

`--sysroot` alone does not isolate distro cross GCC. `JetsonRuntime.cmake`
disables implicit system headers and startup/runtime selection. It selects
target libc/libm/libstdc++/libgcc_s and libc startup objects by explicit paths.
GCC's own intrinsic headers, libgcc.a and crtbegin/crtend are compiler support,
not acquired Jetson inputs; selected-input QA identifies and SHA256-hashes
them separately, along with the compiler driver, cc1plus, assembler and linker.

## Acquisition coordination: required target inputs

Do not substitute the build machine's `/usr/aarch64-linux-gnu` packages.
The acquired manifest/payload needs the selected closure of:

- ARM64 libc development headers under `usr/include`, including
  `stdc-predef.h`, libc bits/gnu headers, and Linux UAPI headers.
- Target libstdc++-13 development headers under `usr/include/c++/13`, with
  `bits/c++config.h` under `usr/include/aarch64-linux-gnu/c++/13`.
- ARM64 libc development/runtime inputs under `usr/lib/aarch64-linux-gnu`
  and `lib/aarch64-linux-gnu`: crt1.o, crti.o, crtn.o, libc.so linker script,
  libc.so.6, libc_nonshared.a, libm.so and its script-selected inputs,
  libgcc_s.so.1 and the ARM64 dynamic loader.
- `usr/lib/gcc/aarch64-linux-gnu/13/libstdc++.so` and its complete symlink
  closure to target libstdc++.so.6. The flat `usr/lib` layout is also supported
  for local build fixtures, not presented as observed Jetson provenance.
- Every actually selected target header/library, including resolved symlink
  destinations, must be recorded in the observed manifest. Extra unrecorded
  bytes in a sysroot are not validated merely by being inside that directory.

## Qualification and retained evidence

Start a fresh build directory with CPATH, C_INCLUDE_PATH, CPLUS_INCLUDE_PATH,
LIBRARY_PATH, GCC_EXEC_PREFIX and COMPILER_PATH unset. Set
`AA_SYSROOT_REQUIRE_OBSERVED=ON`, pair `AA_SYSROOT_MANIFEST` and
`AA_SYSROOT_PAYLOAD`, and set `AA_SYSROOT_ROOTFS` to that same payload.
Use the pre-staged GoogleTest archive; configure performs no downloads.

The root project hash gate runs before `enable_language(C CXX)` and again
before building aa_host_smoke and staged GoogleTest. Build aa_host_smoke and
run `ctest -R 'cross_(contamination|runtime_regressions)' --output-on-failure`.
Actual object-side `*.headers.d` files retain system header dependencies;
`aa_host_smoke.map` retains linker LOAD inputs; `aa_host_smoke.inputs.json`
retains classified selected-input hashes. CTest's
`Testing/Temporary/LastTest.log` retains the selected-input JSON with hashes,
categories, resolved paths, acceptance label and contamination findings.

Local QA uses `build/strict-runtime-qa` and a **synthetic link sysroot** at
`/tmp/opencode/t5root.vTU6fF/strict-fixture-sysroot`, adapted from the existing
local ARM64 runtime fixture. Its separate hash-gate fixture payload is not
the link sysroot, is explicitly labelled `fixture-link-sysroot-not-target`,
and is never target acceptance. The original incomplete `sysroot-a64` fails
readiness for missing target startup/development inputs instead of falling
back. No observed Jetson build or target execution is claimed by this QA.

## Observed offline task-5 acceptance

The later observed qualification uses the frozen
`.local/sysroots/jetson-r39.2.1/rootfs` as both `AA_SYSROOT_ROOTFS` and
`AA_SYSROOT_PAYLOAD`, `toolchains/jetson-sysroot-manifest.json`, and
`AA_SYSROOT_REQUIRE_OBSERVED=ON`. Its manifest digest is
`aa57014c31017139b63b917878a77ee16968aa64b30a10a624f0ba5f68d8d3d9`
(2651 observed inputs). A fresh root preset configure, exact root configure and
build presets, and all 31 host-side CTests passed. `host_cpp20_smoke` was excluded
because its ARM64 executable must not run on the host. Selected-input QA found
454 manifest-recorded target inputs, 5 compiler executables, 9 compiler intrinsic/
support inputs, and 61 project/staged-dependency inputs, with no contamination.

Private cache publication retained actual artifact bytes in a 116930560-byte
content-addressed archive with SHA-256
`8b76a9989118ec803957ad25bee16d3ebb7cdbdf3c1abfd3e75e52b1ade724d8`.
Observed materialization reuse, independent cache determinism, publication reuse,
and verification passed. Package-version and same-version-content drift in copied
offline metadata were rejected with `NEW_VERSION_REQUIRED`. No runtime-path fix,
host target-runtime substitution, target contact, or target execution was needed.

Durable evidence is under `.omo/evidence/jetson-android-auto-receiver/` in the
main workspace: `task-5-sysroot.json`, `task-5-observed-acceptance-driver.py`, and
`task-5-observed-acceptance/`. Earlier fixture/acquisition evidence remains
historical and unchanged. The frozen artifact's capability stays `pending`;
cross-build/cache qualification is external and does not imply target execution
or full receiver/AASDK ABI qualification. LSP diagnostics were requested but the
previously declined basedpyright/biome servers are unavailable.
