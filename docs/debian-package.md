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
without building the receiver binaries. With Debian staging enabled and
`receiver-deb-binaries` built, `deb_package_staged` also installs the CMake
component into a temporary directory, assembles it, and checks the resulting
package. The tests skip on hosts missing the Debian or ELF tools.

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
seconds and allows up to five starts within 60 seconds. Invalid command-line
arguments exit with status 2 and suppress restart.

Once enabled, the receiver starts with the systemd user manager's
`default.target`, including on Xfce. It uses the user session D-Bus and the
provisioned system helper. It runs without a display. An SSH or other
non-graphical login can start the user manager, and enabling linger starts it
at boot and keeps it running after logout. The packaged pairing-only command
serves enrolment and control without starting an AP or projection. A wireless
projection override waits for a consumer before starting either.

Package configuration checks that the shipped default-deny helper policy
is present and reloads udev so the packaged USB rule applies without a reboot.
The package ships no systemd presets or enablement symlinks for either service.
The maintainer scripts leave activation to the operator. After provisioning
and starting the system helper, run
`systemctl --user enable --now aa-receiver.service` as the intended receiver
user. If that user already enabled the unit when it used
`graphical-session.target`, run `systemctl --user reenable aa-receiver.service`
to replace the old enablement link, then
`systemctl --user start aa-receiver.service` to start it in the current session.

On removal, `prerm` stops a running `aa-system-helper.service` before its
default-deny policy is removed, and `postrm` reloads udev. After an upgrade,
restart the helper once you have refreshed the receiver digest.

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

## Refreshing the USB rule

Package configuration warns when the local override is present, including a
masking symlink. For manual installs, back up and move an obsolete override
out of the rules directory before installing, or reload udev and reconnect
the affected device after moving it. The deployment tool backs up and removes
the legacy rule during migration. Package configuration reloads the rules and
triggers only the approved USB devices, their USB interfaces, and their tty
children.
