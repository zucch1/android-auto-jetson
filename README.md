# android-auto-jetson

Public Android Auto receiver targeting NVIDIA Jetson.

**Status: Active implementation with host-tested pipelines and synthetic ARM64 cross-builds.** The repository contains an unprivileged receiver daemon, a privileged system helper, a dummy display consumer, core protocol/transport/media libraries, and host test suites.

## Implementation and qualification status

To prevent confusion between host tests, cross-compilation, and device qualification:

- **Implemented**: Unprivileged wired receiver (`aa-wired-receiver`), privileged helper daemon (`aa-system-helper`), dummy test display (`aa-dummy-display`), H.264 hardware/software consumer decode pipeline, audio pipeline (defaulting to `pulsesink` via `pipewire-pulse`), versioned D-Bus control service (`org.custom.AndroidAutoReceiver1`), approve-once phone trust store (`aa_trust`), and pseudonymizing diagnostics.
- **Host-tested**: Native unit, integration, and recovery test suites pass under host simulation and synthetic fixtures (`ctest --preset host-dev`).
- **Cross-built against fixture**: Public CI cross-compiles for AArch64 against a synthetic fixture sysroot (the fixture cross-build job in `.github/workflows/ci.yml`, `AA_SYSROOT_REQUIRE_OBSERVED=OFF`). This is explicitly **fixture-not-target**, not target qualification. Observed-sysroot cross-compilation acceptance is relayed via `tools/ci/arm64-cross-relay.sh` for exact head commits.
- **Target-qualified**: On-device execution on NVIDIA Jetson hardware (L4T R39.2.1) is governed by live qualification runbooks (`docs/qualification/`); synthetic host tests do not substitute for target acceptance.
- **Real-phone-qualified**: End-to-end projection with a physical phone over USB/AOA is governed by phase-B live QA procedures; host-side fixture passes do not qualify device interoperability.
- **Active blockers**:
  - *Wireless TCP endpoint binding*: A shared WPA2/WPA3 PSK cannot cryptographically bind the incoming TCP connection to the authenticated Bluetooth peer. Production rejects unbound connections with `transport_unauthorized_peer` (`src/app/WirelessLink.cpp`); wireless admission is **BLOCKED** and not delivered in production.
  - *Target development dependency bindings*: While toolchain runtime packages are bound (`deps/manifest.json` status `bound-task5-observed`), required development components (`protobuf`, `Boost`, `OpenSSL`, `libusb`, `GStreamer`, `Qt6`) remain `unbound-no-target-observations`.

## Implemented architecture

- **`aa-wired-receiver`**: Unprivileged receiver daemon running in user session. Owns USB/AOA and TCP transports, AASDK protocol translation, session state machine, media routing, and the user-session D-Bus control service (`org.custom.AndroidAutoReceiver`).
- **`aa-system-helper`**: Privileged daemon managing NetworkManager AP, BlueZ Android Auto RFCOMM profile bootstrap, one-use grants, and caller UID/PID verification.
- **`aa-dummy-display`**: Qt6 test display consuming encoded H.264 access units over a bounded `AF_UNIX` `SOCK_SEQPACKET` socket, with GStreamer hardware decode and a 5 ms precise timer pump (`tools/dummy-display/pump_timer.hpp`) for A/V synchronization.
- **Audio pipeline**: Multi-role audio sink (`aareceiver-audio`) supporting `pulsesink` (default, via `pipewire-pulse` for continuous playback stability) and `pipewiresink`, with A/V sync gating.
- **D-Bus control contract**: Versioned interface `org.custom.AndroidAutoReceiver1` (12 methods, 5 signals) with peer credential verification and pairing gates.
- **Trust store**: Persistent identity allowlist (`aa_trust`) with atomic file persistence and fail-closed admission; wired identifiers represent physical-access trust rather than cryptographic proof and are spoofable (`include/aa/trust/Store.hpp:5-23`).
- **Diagnostics**: Structured JSON event telemetry with salted identifier pseudonymization (`id-<hex>`).

## How to build and test

### Prerequisites

- CMake >= 3.20, Ninja, Python >= 3.12, C++20 compiler (GCC or Clang).
- System development packages: `libprotobuf-dev`, `protobuf-compiler`, `libboost-system-dev`, `libboost-log-dev`, `libssl-dev`, `libusb-1.0-0-dev`, `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `gstreamer1.0-tools`, `gstreamer1.0-plugins-good`, `gstreamer1.0-plugins-bad`, `gstreamer1.0-pipewire`, `pipewire-bin`, `wireplumber`, `pipewire-pulse`, `pulseaudio-utils`, `dbus-daemon`, `qt6-base-dev`, `valgrind`, `pkg-config`, `binutils` (matching `.github/workflows/ci.yml`).
- Pre-staged locked GoogleTest v1.15.2 archive (configure never downloads dependencies).
- Audio runtime: Audio pipeline route tests require an active user-session PipeWire audio runtime with default sink resolution and PulseAudio compatibility (see the audio runtime step in `.github/workflows/ci.yml`).

### Host build

```sh
set -euo pipefail
export AA_GOOGLETEST_ARCHIVE="${AA_GOOGLETEST_ARCHIVE:-.local/dependencies/googletest-v1.15.2.tar.gz}"

# Verify dependency manifest integrity
python3 -B tools/deps/check_manifest.py --lock deps/manifest.lock --archive "$AA_GOOGLETEST_ARCHIVE"

# Configure, build and test host preset (AA_GOOGLETEST_ARCHIVE is optional if staged at .local/dependencies/)
cmake --preset host-dev
cmake --build --preset host-dev
ctest --preset host-dev --output-on-failure
```

### Reproducing public CI checks locally

The public CI workflow ([`.github/workflows/ci.yml`](.github/workflows/ci.yml), which is the authority for active CI job names and steps) executes independent checks, including the following:

1. **`ci/host-build-test`**: `cmake --build --preset host-dev && ctest --preset host-dev --output-on-failure` (PR runs defer `ipc-bench-happy-600` when benchmark sources and build commands are unchanged vs base).
2. **`ci/provenance-lint`**: `tools/provenance/check.sh` and `for case in baseline overlay content header extra generated license gpl inventory listing symlink missing mode; do python3 -B tests/provenance/scenarios.py "$PWD" "$case"; done`.
3. **`ci/decode-overlay-cross`**: Probe, AArch64 ELF and bound overlay provenance, followed by full root cross provenance with an external overlay (commands below).
4. **ARM64 fixture cross-build**: A generated synthetic fixture sysroot with `AA_SYSROOT_REQUIRE_OBSERVED=OFF`; explicitly **fixture-not-target**. The workflow file names the job. For exact-head target cross-acceptance, the maintainer runs the observed-sysroot smoke (`AA_SYSROOT_REQUIRE_OBSERVED=ON`) with `tools/ci/arm64-cross-relay.sh`, which posts the `ci/arm64-cross-build` commit status for that exact SHA.

Run the following setup from the repository root in Bash. The cross checks require
`ninja-build`, `cmake`, `g++-aarch64-linux-gnu`, `pkg-config`, and `binutils`;
the decode checks also require `libgstreamer1.0-dev` and
`libgstreamer-plugins-base1.0-dev` (the workflow's “Provision public synthetic
cross fixture dependencies” step). The archive is fetched and verified outside
configure, as in the workflow's “Fetch and verify locked GoogleTest archive” steps.

```bash
set -euo pipefail
export AA_CI_SCRATCH="$(mktemp -d /dev/shm/aa-ci.XXXXXXXX)"
export AA_GOOGLETEST_ARCHIVE="$AA_CI_SCRATCH/googletest-v1.15.2.tar.gz"
curl --fail --location --retry 3 --proto '=https' --proto-redir '=https' \
  https://codeload.github.com/google/googletest/tar.gz/refs/tags/v1.15.2 \
  --output "$AA_GOOGLETEST_ARCHIVE"
python3 -B tools/deps/check_manifest.py --lock deps/manifest.lock --archive "$AA_GOOGLETEST_ARCHIVE"
```

Run both decode checks in that shell. These match the workflow steps “Require
AVAILABLE probe, AArch64 ELF and bound overlay provenance” and “Require full root
cross provenance with an external overlay”, using local scratch paths.

```bash
python3 -B tests/media/cross_decode_overlay.py --scratch "$AA_CI_SCRATCH/decode-overlay-cross"
python3 -B tests/media/cross_decode_overlay.py --root-project \
  --archive "$AA_GOOGLETEST_ARCHIVE" --scratch "$AA_CI_SCRATCH/decode-root-cross"
```

Generate and bind the ARM64 fixture in the same shell. This adapts the fixture
cross-build job's “Generate fixture sysroot into RUNNER_TEMP (fixture-not-target)”
step in [`.github/workflows/ci.yml`](.github/workflows/ci.yml): the local scratch
variable replaces `RUNNER_TEMP`, and sourcing a shell file replaces `GITHUB_ENV`.

```bash
python3 -B - <<'PYTHON'
import os
import shlex
import sys
from pathlib import Path

sys.path.insert(0, 'tests/media')
from decode_cross_fixture import make_sysroot

base = Path(os.environ['AA_CI_SCRATCH']) / 'arm64-cross-fixture'
base.mkdir(parents=True, exist_ok=False)
rootfs, manifest = make_sysroot(base)
if not rootfs.is_dir() or not manifest.is_file():
    raise SystemExit('FIXTURE_SYSROOT_GENERATION_FAILED')
cache = base / 'build-binding'
cache.mkdir()
bindings = {
    'AA_SYSROOT_ROOTFS': str(rootfs),
    'AA_SYSROOT_PAYLOAD': str(rootfs),
    'AA_SYSROOT_MANIFEST': str(manifest),
    'AA_SYSROOT_CACHE': str(cache),
    'AA_SYSROOT_REQUIRE_OBSERVED': 'OFF',
}
with (base / 'fixture.env').open('w', encoding='utf-8') as environment:
    for name, value in bindings.items():
        environment.write(f'export {name}={shlex.quote(value)}\n')
PYTHON
source "$AA_CI_SCRATCH/arm64-cross-fixture/fixture.env"
```

**Workflow excerpt, not run locally:** Continue in that shell with the fixture
cross-build job's “Configure and cross-build against the fixture sysroot” and
“Cross-build IPC libraries explicitly and verify the cross inventory” steps in
[`.github/workflows/ci.yml`](.github/workflows/ci.yml). These build commands were
not executed during this documentation fix round; fixture generation and both
decode checks were exercised separately. Fixture results do not qualify a target.

```bash
mkdir -p /tmp/opencode
export PKG_CONFIG_PATH=
test -d "${AA_SYSROOT_ROOTFS:?AA_SYSROOT_ROOTFS must be set}"
test -f "${AA_SYSROOT_MANIFEST:?AA_SYSROOT_MANIFEST must be set}"
test "${AA_SYSROOT_REQUIRE_OBSERVED:?AA_SYSROOT_REQUIRE_OBSERVED must be set}" = "OFF"
cmake --preset jetson-aarch64
cmake --build --preset jetson-aarch64
cmake --build --preset jetson-aarch64 --target aa_ipc_control aa_ipc_dbus
python3 -B tests/dbus/assert_dbus_coverage.py --cross build/jetson-aarch64
```

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
