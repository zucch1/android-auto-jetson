# Host foundation builds

Requires CMake >= 3.20, Ninja, Python >= 3.12, and a C/C++ compiler with C++20 support (GCC or Clang for the warning policy and AddressSanitizer).

## Dependencies

The native host build and tests require system development packages (matching the package-install step of `.github/workflows/ci.yml`):

- **Build tools**: `cmake`, `ninja-build`, `pkg-config`, `binutils`, `g++` (or `clang`).
- **Protobuf**: `libprotobuf-dev`, `protobuf-compiler` (versions must match; tested with 3.21.12).
- **Boost**: `libboost-system-dev`, `libboost-log-dev` (tested with 1.83.0).
- **Security & USB**: `libssl-dev` (OpenSSL 3.0), `libusb-1.0-0-dev`.
- **GStreamer**: `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `gstreamer1.0-tools`, `gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad`, `gstreamer1.0-pipewire`.
- **Audio & IPC**: `pipewire-bin`, `wireplumber`, `pipewire-pulse`, `pulseaudio-utils`, `dbus-daemon`.
- **Display**: `qt6-base-dev` (Qt 6.4+).
- **Test tooling**: `valgrind`.

### Audio runtime setup for tests

The audio pipeline route tests open the default output of the user session (`pipewiresink` auto-linked by the session manager and `pulsesink` driving against a Pulse server). Tests require an active user-session audio runtime with PipeWire, WirePlumber, and PipeWire-Pulse daemons, and a resolvable default sink (`@DEFAULT_AUDIO_SINK@`). In headless or container environments without an existing session, start the runtime daemons following the “Start user-session audio runtime for the audio pipeline tests” step in [`.github/workflows/ci.yml`](../.github/workflows/ci.yml).

### Dependency staging

Supply the pinned GoogleTest v1.15.2 archive offline. `CMakeLists.txt:6-9` resolves `AA_GOOGLETEST_ARCHIVE` in order:
1. An existing CMake cache value (`-DAA_GOOGLETEST_ARCHIVE=...` or from a gitignored `CMakeUserPresets.json`),
2. The environment variable `$AA_GOOGLETEST_ARCHIVE`,
3. The default location `${CMAKE_SOURCE_DIR}/.local/dependencies/googletest-v1.15.2.tar.gz` (gitignored).

The archive hashlock and original-source provenance checks always run before staging. Nothing is downloaded.

## Building and testing presets

For each of `host-dev`, `host-release`, and `host-asan`, run the commands below,
substituting that preset name for `host-dev` throughout:

```sh
set -euo pipefail

# Configure host preset (archive override is optional if pre-staged at default location)
cmake --preset host-dev

# Or pass an explicit archive location override:
# cmake --preset host-dev -DAA_GOOGLETEST_ARCHIVE=/path/to/googletest-v1.15.2.tar.gz

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
clang-format --dry-run --Werror tests/host/smoke.cpp
clang-tidy -p build/host-dev tests/host/smoke.cpp
clangd --check=tests/host/smoke.cpp --compile-commands-dir=build/host-dev
```

`.clangd` directs editor diagnostics to the development compilation database.
