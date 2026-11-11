# android-auto-jetson

Public Android Auto receiver targeting NVIDIA Jetson.

**Status: Active implementation with host-tested pipelines and synthetic ARM64 cross-builds.** The repository contains an unprivileged receiver daemon, a privileged system helper, a dummy display consumer, core protocol/transport/media libraries, and host test suites.

## Implementation and qualification status

To prevent confusion between host tests, cross-compilation, and device qualification:

- **Implemented**: Unprivileged wired receiver (`aa-wired-receiver`), privileged helper daemon (`aa-system-helper`), dummy test display (`aa-dummy-display`), H.264 hardware/software consumer decode pipeline, audio pipeline (defaulting to `pulsesink` via `pipewire-pulse`), versioned D-Bus control service (`org.custom.AndroidAutoReceiver1`), approve-once phone trust store (`aa_trust`), and pseudonymizing diagnostics.
- **Host-tested**: Native unit, integration, and recovery test suites pass under host simulation and synthetic fixtures (`ctest --preset host-dev`).
- **Cross-built against fixture**: Public CI cross-compiles for AArch64 against a synthetic fixture sysroot (`ci/arm64-cross-build`, `AA_SYSROOT_REQUIRE_OBSERVED=OFF`). This is explicitly **fixture-not-target**, not target qualification.
- **Target-qualified**: On-device execution on NVIDIA Jetson hardware (L4T R39.2.1) is governed by live qualification runbooks (`docs/qualification/`); synthetic host tests do not substitute for target acceptance.
- **Real-phone-qualified**: End-to-end projection with a physical phone over USB/AOA is governed by phase-B live QA procedures; host-side fixture passes do not qualify device interoperability.
- **Active blockers**:
  - *Wireless TCP endpoint binding*: A shared WPA2/WPA3 PSK cannot cryptographically bind the incoming TCP connection to the authenticated Bluetooth peer. Production rejects unbound connections (`src/app/HandoffEligibility.cpp`); wireless admission is **BLOCKED** and not delivered in production.
  - *Target development dependency bindings*: While toolchain runtime packages are bound (`deps/manifest.json` status `bound-task5-observed`), required development components (`protobuf`, `Boost`, `OpenSSL`, `libusb`, `GStreamer`, `Qt6`) remain `unbound-no-target-observations`.

## Implemented architecture

- **`aa-wired-receiver`**: Unprivileged receiver daemon running in user session. Owns USB/AOA and TCP transports, AASDK protocol translation, session state machine, media routing, and the user-session D-Bus control service (`org.custom.AndroidAutoReceiver`).
- **`aa-system-helper`**: Privileged daemon managing NetworkManager AP, BlueZ Android Auto RFCOMM profile bootstrap, one-use token authentication, and caller UID/PID verification.
- **`aa-dummy-display`**: Qt6 test display consuming encoded H.264 access units over a bounded `AF_UNIX` `SOCK_SEQPACKET` socket, with GStreamer hardware decode and a 5 ms precise timer pump (`tools/dummy-display/pump_timer.hpp`) for A/V synchronization.
- **Audio pipeline**: Multi-role audio sink (`aareceiver-audio`) supporting `pulsesink` (default, via `pipewire-pulse` for continuous playback stability) and `pipewiresink`, with A/V sync gating.
- **D-Bus control contract**: Versioned interface `org.custom.AndroidAutoReceiver1` (12 methods, 5 signals) with peer credential verification and pairing gates.
- **Trust store**: Approve-once phone store (`aa_trust`) with HMAC-SHA256 tokens, atomic file persistence, and fail-closed admission.
- **Diagnostics**: Structured JSON event telemetry with salted identifier pseudonymization (`id-<hex>`).

## How to build and test

### Prerequisites

- CMake >= 3.20, Ninja, Python >= 3.12, C++20 compiler (GCC or Clang).
- System development packages: `libprotobuf-dev`, `protobuf-compiler`, `libboost-system-dev`, `libboost-log-dev`, `libssl-dev`, `libusb-1.0-0-dev`, `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `gstreamer1.0-tools`, `gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad`, `pipewire-pulse`, `dbus-daemon`, `qt6-base-dev`, `valgrind`, `pkg-config`, `binutils`.
- Pre-staged locked GoogleTest v1.15.2 archive (configure never downloads dependencies).

### Host build

```sh
set -euo pipefail
export AA_GOOGLETEST_ARCHIVE=/path/to/googletest-v1.15.2.tar.gz

# Verify dependency manifest integrity
python3 -B tools/deps/check_manifest.py --lock deps/manifest.lock --archive "$AA_GOOGLETEST_ARCHIVE"

# Configure, build and test host preset
cmake --preset host-dev -DAA_GOOGLETEST_ARCHIVE="$AA_GOOGLETEST_ARCHIVE"
cmake --build --preset host-dev
ctest --preset host-dev --output-on-failure
```

### Reproducing public CI checks locally

The public CI workflow (`.github/workflows/ci.yml`) executes four independent checks:

1. **`ci/host-build-test`**: `cmake --build --preset host-dev && ctest --preset host-dev --output-on-failure` (PR runs defer `ipc-bench-happy-600` when benchmark sources and build commands are unchanged vs base).
2. **`ci/provenance-lint`**: `tools/provenance/check.sh` and `python3 -B tests/provenance/scenarios.py "$PWD" <scenario>`.
3. **`ci/decode-overlay-cross`**: `python3 -B tests/media/cross_decode_overlay.py`.
4. **`ci/arm64-cross-build`**: `cmake --preset jetson-aarch64 && cmake --build --preset jetson-aarch64` (against generated synthetic fixture sysroot; explicitly **fixture-not-target**).

## Documentation directory

- [ARCHITECTURE.md](ARCHITECTURE.md) - System processes, module architecture, threading model, include boundaries, and contract budgets.
- [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) - Contribution workflow, worktree ownership, and CI job definitions.
- [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) - Pinned dependencies, manifest locking, GoogleTest staging, and target binding status.
- [docs/HOST_BUILD.md](docs/HOST_BUILD.md) - Host building instructions, presets, and development toolchain requirements.
- [docs/SYSROOT.md](docs/SYSROOT.md) - Jetson sysroot manifest format, offline materialization CLI, and provenance rules.
- [SECURITY.md](SECURITY.md) - Security policy, vulnerability reporting, and secrets management.
- [docs/dbus/README.md](docs/dbus/README.md) - Session D-Bus interface contract (`org.custom.AndroidAutoReceiver1`).
- [docs/system-helper.md](docs/system-helper.md) - Composed privileged helper architecture (NetworkManager AP and BlueZ).
- [docs/networkmanager-ap.md](docs/networkmanager-ap.md) - Dedicated NetworkManager AP configuration and regulatory policy.
- [docs/task36-bluez.md](docs/task36-bluez.md) - BlueZ RFCOMM pairing and service record configuration.
- [docs/wireless-handoff.md](docs/wireless-handoff.md) - Wireless bootstrap, TCP handoff, and endpoint binding blockers.
- [docs/tls-credentials.md](docs/tls-credentials.md) - TLS posture, head-unit credentials, and approved-phone validation.
- [docs/capability-gate.md](docs/capability-gate.md) - Capability gate report and qualification status ledger.
- [docs/qualification/](docs/qualification/) - Target live QA runbooks (wired live QA, recovery QA, perf/soak runbook).

## Governance

- Default branch: `main`. Protected: no force-push, no deletion.
- Pull requests merge **squash-only**; merge commits and rebase merges are disabled at repository level.
- **Zero required approvals** on `main` (checks gate, not reviewers).
- Branch naming convention: `aa/<topic>` for work branches applied on top of `main`.
- Author identity is repository-local: `Newton <59395246+zucch1@users.noreply.github.com>`.

## License

Project-owned code is **GPL-3.0-or-later**; see [LICENSE](LICENSE) for the full GNU General Public License v3 text, and the per-file "or any later version" notices that land with it. Third-party components are licensed as documented in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): AASDK carries per-file GPL-3.0-or-later grants (upstream ships no root LICENSE), and the OAA reference subset has unresolved version scope (explicit per-file SPDX headers say GPL-3.0-or-later; the upstream aggregate reads as GPL version 3). No blanket "or any later version" assertion is made for third-party material with unresolved version scope. The receiver combination is distributed under **GPL version 3**, a version permitted by every reading.
