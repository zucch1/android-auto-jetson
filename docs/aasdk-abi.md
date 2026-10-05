# AASDK language-standard decision

Decision: **`aasdk-cpp20`** (native qualification, not target qualification).

Pinned OpenCarDev AASDK `9bf6adf933665dee26532201719fac14a047ccf1`
is built from the existing immutable effective stage. Its 566 original vendor
files remain unchanged; only the previously locked GoogleTest patch is applied
by `tools/deps/stage.py`. No new vendor patch or source pin is needed.

## Build boundary

`cmake/AasdkAbi.cmake` adds the complete upstream production build, including
all 59 production `.cpp` sources and all 254 generated protobuf `.pb.cc` sources.
Upstream root and protobuf directories both declare C++17. The integration
sets **both actual targets** to required C++20 without GNU extensions; a CTest
checks every relevant compile command, not merely the parent CMake default.
Project-owned host smoke and ABI consumer targets remain C++20.

The native build uses system protobuf/protoc with equal versions (qualified
with `3.21.12`), Boost `1.83.0`, OpenSSL `3.0.13`, and libusb `1.0.27`.
`SKIP_BUILD_PROTOBUF` and `SKIP_BUILD_ABSL` are forced on. Configure never
fetches dependencies; GoogleTest must be supplied as its verified locked archive.
The normal native host presets build AASDK and generated protobuf as upstream
shared libraries with `--no-undefined` linker checks. No C++17 isolation was
necessary: production sources compile, link and run with C++20.

Existing vendor warnings (including libusb flexible arrays, pessimizing moves,
and unused parameters) are not evidence of a standard/ABI blocker. They remain
visible rather than suppressed or "fixed" by changing TLS/protocol behavior.
This decision is not a warning-free, TLS, handshake or phone qualification.

## Executable qualification

```sh
cmake --preset host-dev -DAA_GOOGLETEST_ARCHIVE=/path/to/verified/archive.tar.gz
cmake --build --preset host-dev
ctest --test-dir build/host-dev -R aasdk_abi --output-on-failure -V
ctest --preset host-dev --output-on-failure
```

- `aasdk_abi_consumer` constructs real `SSLWrapper`/`Cryptor` objects, calls
  explicitly qualified out-of-line methods, and round-trips a non-empty generated
  protobuf message. It does not initialize a TLS session or load credentials.
- `aasdk_abi_symbols` runs `nm -C` on the actual consumer and both libraries,
  checks production definitions and consumer imports, then uses `ldd -r` to
  prove every relocation resolves. Undefined dynamic imports into system
  OpenSSL, Boost, protobuf and the C++ runtime are legitimate; an unresolved
  SDK boundary or missing system provider fails. Reports are retained alongside
  ignored build binaries as `.nm-defined.txt`, `.nm-undefined.txt`, `.ldd.txt`.
- `aasdk_abi_standard_negative` builds and runs the **same consumer** in a
  temporary C++20 target, then requires its C++17 variant to fail compilation
  on `std::span`/`std::same_as`. Both fixture command outputs are retained.
- `aasdk_abi_compile_flags` requires complete staged production/schema coverage
  and actual `-std=c++20` commands for SDK, generated code and owned consumers.

CI provisions native development packages, prints installed package versions,
and runs the real ABI suite in addition to the existing host/provenance/deps
tests. No generated source, binaries, evidence or development-prefix bytes
belong in Git.

## Cross-build limitation

Cross builds intentionally keep the accepted cross-smoke path. The frozen
private sysroot contains compiler/runtime inputs, not qualified AASDK
development libraries. The integration returns before native dependency
discovery when `CMAKE_CROSSCOMPILING` is true and reports that ABI qualification
is unavailable. No host libraries are injected into the cross build. This
record makes no ARM64 AASDK, Jetson runtime, installation or packaging claim.
If a later supported toolchain demonstrates a genuine C++20 blocker, any
C++17 fallback requires new evidence and an isolated static boundary; it is not
a selectable workaround in this native qualification.
