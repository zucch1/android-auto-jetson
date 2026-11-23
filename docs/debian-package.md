# Building the Android Auto Receiver Debian package

Build the ARM64 package in an Ubuntu 24.04 ARM64 environment. A native
ARM64 builder or an ARM64 Ubuntu userspace under emulation can run the
full `host-release` preset. Use the build dependencies and pre-staged
GoogleTest archive described in [HOST_BUILD.md](HOST_BUILD.md).
The `jetson-aarch64` preset checks the pinned Jetson sysroot with a smaller
cross-build target set. The package needs the full wired and wireless
receivers, system helper, and Qt display, which are native-only CMake targets.

From the repository root on the ARM64 builder:

```sh
export SOURCE_DATE_EPOCH="$(git show -s --format=%ct HEAD)"
cmake --preset host-release -DAA_ENABLE_DEB_PACKAGE=ON
cmake --build --preset host-release --target receiver-deb-binaries
stage="$(mktemp -d)"
DESTDIR="$stage" cmake --install build/host-release --prefix /usr \
  --component receiver-deb --strip
tools/packaging/build-deb.py "$stage" build/dist
rm -r -- "$stage"
```

The output is `build/dist/android-auto-receiver_<version>_arm64.deb`.
A plain `vMAJOR.MINOR.PATCH` Git tag determines a release version; pre-release
tags such as `v1.0.0-rc1` are ignored. Later commits add
`+git<distance>.g<hash>`. Untagged history uses `0.0.0+git<count>.g<hash>`.
Use a clean, detached checkout with full Git history and tags for a release.
Assembly is reproducible from identical staged files and a fixed Git revision.
With `AA_ENABLE_DEB_PACKAGE=ON`, source, build and every `CMAKE_PREFIX_PATH`
dependency prefix are remapped in embedded file paths, so a private dependency
prefix (as on the Jetson) never reaches the package.
The commit timestamp is the default `SOURCE_DATE_EPOCH`. The package uses
Ubuntu 24.04 runtime dependencies. The NVIDIA GStreamer plugin, if required,
comes from the Jetson software stack managed by the operator.

For local verification on an x86-64 Ubuntu builder, run the same build and
staging steps, then pass `--arch amd64` to the assembler. From either native
architecture, CTest can exercise package assembly with small ELF fixtures
without building the receiver binaries:

```sh
ctest --preset host-release -R '^deb_package_' --output-on-failure
```

## Inspecting the artifact

```sh
deb="build/dist/android-auto-receiver_VERSION_arm64.deb"
dpkg-deb --info "$deb"
dpkg-deb --contents "$deb"
lintian --fail-on error "$deb"   # optional
```

The package contains the two receivers, wired enrolment CLI, privileged
helper, consumer decode archive, dummy display, USB and decode probes,
IPC benchmark, D-Bus policy and interface definitions, scoped udev policy,
and operator docs. The bundled AASDK/Protobuf libraries use a private
library directory. Build inputs and any operator state stay outside the
archive. The AP example is under the package documentation directory.

## Service files and provisioning

The receiver unit is installed in `/usr/lib/systemd/user`. Its reference
command starts the wireless receiver in pairing-only mode and uses the
user manager's state directory. The unit restarts on failure after two
seconds and allows up to five starts within 60 seconds. The helper is
a separate system unit.
Package configuration checks that the shipped default-deny helper policy
is present and reloads udev so the packaged USB rule applies without a reboot.
Both services require explicit operator activation; the maintainer scripts
never enable or start a unit. On removal, `prerm` stops an already-running
`aa-system-helper.service` before its default-deny policy is removed, and
`postrm` reloads udev. An upgrade leaves a running helper alone: restart it
after refreshing the receiver digest.

[System helper provisioning](system-helper.md#explicit-fail-closed-provisioning-not-installed-by-this-task)
covers the session UID, receiver executable digest, and per-user D-Bus
permission. [NetworkManager AP setup](networkmanager-ap.md) and
[TLS credential handling](tls-credentials.md) cover operator-owned
configuration. Calculate the helper digest from the **installed** receiver
after each upgrade or rollback. A per-user systemd drop-in can replace
`ExecStart` with the intended receiver CLI and its arguments once provisioning
is complete. The historical standalone USB-policy installer may have left a
rule under `/etc/udev/rules.d` with the same name. Check that override during
deployment because udev gives it precedence over the packaged rule.

Jetson install, upgrade, and rollback are documented in [deploy.md](deploy.md)
(tracked in [#190](https://github.com/zucch1/android-auto-jetson/issues/190)).
Installed-system qualification is tracked in [#191](https://github.com/zucch1/android-auto-jetson/issues/191).
