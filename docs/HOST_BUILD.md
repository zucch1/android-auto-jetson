# Host foundation builds

Requires CMake >= 3.20, Ninja, Python >= 3.12, and a C/C++ compiler with C++20 support (GCC or Clang for the warning policy and AddressSanitizer).

## Dependencies

The native host build and tests require system development packages (matching `.github/workflows/ci.yml`):

- **Build tools**: `cmake`, `ninja-build`, `pkg-config`, `binutils`, `g++` (or `clang`).
- **Protobuf**: `libprotobuf-dev`, `protobuf-compiler` (versions must match; tested with 3.21.12).
- **Boost**: `libboost-system-dev`, `libboost-log-dev` (tested with 1.83.0).
- **Security & USB**: `libssl-dev` (OpenSSL 3.0), `libusb-1.0-0-dev`.
- **GStreamer**: `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `gstreamer1.0-tools`, `gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad`.
- **Audio & IPC**: `pipewire-pulse`, `pulseaudio-utils`, `dbus-daemon`.
- **Display**: `qt6-base-dev` (Qt 6.4+).
- **Test tooling**: `valgrind`.

Supply the pinned GoogleTest v1.15.2 archive offline via `AA_GOOGLETEST_ARCHIVE=/path/to/googletest-v1.15.2.tar.gz` in the environment, or place it at `.local/dependencies/googletest-v1.15.2.tar.gz` (gitignored). The archive hashlock and original-source provenance checks always run before staging. Nothing is downloaded. A cache override in a gitignored `CMakeUserPresets.json` may also set `AA_GOOGLETEST_ARCHIVE`; an existing cache value takes precedence over the environment.

## Building and testing presets

For each of `host-dev`, `host-release`, and `host-asan`, run:

```sh
set -euo pipefail
cmake --preset host-dev -DAA_GOOGLETEST_ARCHIVE="$AA_GOOGLETEST_ARCHIVE"
cmake --build --preset host-dev
ctest --preset host-dev --output-on-failure
```

Each preset has a separate ignored `build/<preset>/` directory and exports `compile_commands.json`. The host build compiles the full receiver suite and test targets:

- GoogleTest / GoogleMock staged dependency targets
- AASDK ABI compatibility and consumer verification (`aa_aasdk_abi`)
- Core runtime (`aa_core`)
- Protocol adapter (`aa_protocol_adapter`)
- Transport library (`aa_transport`, `aa_transport_usb`, `aa_transport_tls`)
- Session state machine and lifecycle (`aa_session`)
- Channel services and capability negotiators (`aa_channels`)
- Multi-role audio pipeline (`aareceiver-audio`)
- Consumer video decode pipeline (`aareceiver-consumer`)
- Display library and test application (`aareceiver-display`, `aa-dummy-display`)
- Input and sensor services (`aa_input`, `aa_input_service`)
- D-Bus control contract and bus adapter (`aa_ipc_control`, `aa_ipc_dbus`)
- Approved-phone trust store (`aa_trust`)
- Privileged system helper (`aa_system_helper`, `aa-system-helper`)
- Wired projection receiver (`aa_wired`, `aa-wired-receiver`)
- Wireless handoff and link orchestration (`aa_wireless`)
- Structured diagnostics and privacy logging (`aa_diagnostics`)
- Deterministic session replay (`aa_replay`)
- Performance and probe tools (`aa-ipc-bench`, `aa-decode-probe`, `aa-usb-aoa-probe`)
- Unit, integration, target-safety, and recovery test suites

ASAN (`host-asan`) adds compile and link instrumentation only to project-owned code; vendor standards are untouched.

## Targeted test execution

Run specific test suites with CTest regex filters:

```sh
# Architecture include boundary verification
ctest --preset host-dev -R architecture_boundary --output-on-failure

# AASDK ABI qualification suite
ctest --preset host-dev -R aasdk_abi --output-on-failure

# Wired end-to-end integration tests
ctest --preset host-dev -R wired_e2e --output-on-failure

# Target safety test suite
ctest --preset host-dev -R target_safety --output-on-failure
```

## Static analysis and linting

Run format and tidy checks on project-owned sources:

```sh
set -euo pipefail
clang-format --dry-run --Werror src/core/*.cpp include/aa/core/*.hpp
clang-tidy -p build/host-dev src/core/Error.cpp
clangd --check=src/core/Error.cpp --compile-commands-dir=build/host-dev
```

`.clangd` directs editor diagnostics to the development compilation database.
