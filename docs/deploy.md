# Deploying, upgrading, and rolling back `android-auto-receiver` over SSH

`tools/deploy/aa-deploy.sh` (implemented in `tools/deploy/aa_deploy.py`) deploys a
built `arm64` `.deb` package (see [debian-package.md](debian-package.md)) from the
development host to the target Jetson over SSH, replacing the historical ad-hoc
`~/aa-build/deploy-*.sh` script.

## Prerequisites on the target

Before running `aa-deploy.sh`, the operator must already have provisioned:

1. Python 3 and passwordless `sudo -n` for the SSH user on the target.
2. Root-owned, mode `0600` `/etc/android-auto-receiver/ap.conf` containing a valid
   `v1` line with `jurisdiction-confirmed` (see [networkmanager-ap.md](networkmanager-ap.md)).
   `aa-deploy.sh` validates the schema and jurisdiction match on the target without
   ever printing `ap.conf` contents, passphrases, credentials, or trust stores.
3. Root-owned, mode `0600` `/etc/android-auto-receiver/session.uid` and the
   per-user D-Bus delivery policy fragment under `/etc/dbus-1/system.d/` (see
   [system-helper.md](system-helper.md#explicit-fail-closed-provisioning-not-installed-by-this-task)).
4. No active measurement run (`~/MEASUREMENT_IN_PROGRESS` must be absent).

## Usage

### Dry run (inspect every command without touching the target)

```sh
tools/deploy/aa-deploy.sh zucchi@192.168.55.3 \
  build/dist/android-auto-receiver_<version>_arm64.deb \
  --confirm-jurisdiction US \
  --dry-run
```

### Install or upgrade

```sh
tools/deploy/aa-deploy.sh zucchi@192.168.55.3 \
  build/dist/android-auto-receiver_<version>_arm64.deb \
  --confirm-jurisdiction US
```

`--confirm-jurisdiction CC` is mandatory on every install or upgrade and must
match the two-letter regulatory country code in `/etc/android-auto-receiver/ap.conf`.

### Rollback

```sh
tools/deploy/aa-deploy.sh zucchi@192.168.55.3 --rollback
```

Pass `--dry-run` with `--rollback` to inspect the rollback commands first.
Rollback uses a minimal preflight (target architecture, deploy state directory
present, and no `~/MEASUREMENT_IN_PROGRESS` marker). If the pre-rollback
infotainment snapshot fails or the directory is missing, rollback warns and
proceeds. With a usable baseline it still checks the post-rollback snapshot.
Apt or helper restart failures retain the rollback record for a retry.

## What `aa-deploy.sh` does

1. **Local package validation:** verifies the `.deb` is `Package: android-auto-receiver`,
   `Architecture: arm64`, with a valid `Version`, and computes its SHA-256.
2. **Remote preflight (fail-closed, read-only):**
   - Refuses if `~/MEASUREMENT_IN_PROGRESS` exists.
   - Verifies `dpkg --print-architecture` is `arm64`, `uname -m` is `aarch64`, and
     `/etc/os-release` is Ubuntu `24.04`.
   - Verifies free disk space on `/`, `/var`, and the state directory parent is at
     least `--min-free-mb` (default `512` MiB).
   - Opens `/etc` and `/etc/android-auto-receiver` component by component and
     applies the helper's ownership and permission checks. Validates `ap.conf`
     and `session.uid` against the helper's file and byte-level parsing rules.
     `ap.conf` must also match `--confirm-jurisdiction`. Diagnostics omit file contents.
3. **Infotainment pre-snapshot (read-only):** records `git -C ~/Desktop/infotainment rev-parse HEAD`,
   a SHA-256 of `git --no-optional-locks status --porcelain`, and a deterministic SHA-256 of every
   non-`.git` regular file in `~/Desktop/infotainment`.
4. **Package-set recording before package-manager activity:** creates a root-owned
   timestamped run directory under `/var/lib/android-auto-receiver/deploy/runs/` and
   saves `dpkg-query -W` to `dpkg-query-before.txt` before running any `apt-get` or
   `dpkg` operation.
5. **Private staging, transfer, and dependency check:** creates a private `0700`
   `mktemp` directory on the target, copies the `.deb` via `scp`, verifies its
   SHA-256 on the target, and checks `Depends` satisfiability with `apt-get -s -y install`
   before mutating anything.
6. **Package install and rollback state preservation:** checks the installed
   package against `current.env` and verifies its cached archive before upgrading.
   An installed package with missing or inconsistent deployment metadata requires
   operator recovery before deploy can proceed. Cached archives are immutable per
   Debian version. Build with a new version when the bytes change. Reinstalling the
   exact same package preserves `previous.env`.
7. **Legacy ad-hoc migration:** backs up the artifacts below in the deploy run's
   `legacy/` directory before invoking APT. Backup errors abort installation.
   After installation succeeds, deploy publishes the rollback record and removes
   the shadowing config:
   - `/etc/udev/rules.d/70-android-auto-receiver.rules` (shadows the packaged
     `/usr/lib/udev/rules.d/70-android-auto-receiver.rules`).
   - `/etc/systemd/system/aa-system-helper.service` (shadows the packaged
     `/usr/lib/systemd/system/aa-system-helper.service`). For a unit with
     `ExecStart=/usr/libexec/aa-system-helper`, deploy also saves the original
     executable and rollback restores it before restoring the unit. Other units
     need one literal absolute `ExecStart` path and its executable must remain
     available. Arrange manual recovery for custom unit layouts before migration.
   - `/etc/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf` (duplicates the
     packaged `/usr/share/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf`).
     Operator per-user allow fragments
     (`/etc/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.receiver-*.conf`) are
     never touched.
   - The legacy `receiver.sha256`. After rollback, point the session launcher
     back to the legacy receiver if needed. Keep the original `~/aa-build/bin`
     executables and their launcher configuration until rollback is no longer
     needed. Deploy logs detected legacy binaries in `migration.log`.
8. **Receiver digest:** computes the SHA-256 of the **installed**
   `/usr/bin/aa-wireless-receiver`, and atomically writes the 64-hex digest (with
   trailing LF, mode `0600`, `root:root`) to `/etc/android-auto-receiver/receiver.sha256`.
9. **Service reload and conditional restart:** runs `systemctl daemon-reload` and
   restarts `aa-system-helper.service` **only** if it was already active or enabled
   before the deploy. Never enables or starts an inactive/disabled unit.
10. **Post-deploy verification:** records `dpkg-query -W` to `dpkg-query-after.txt`
    and re-hashes `~/Desktop/infotainment`, failing if `HEAD`, `git status --porcelain`,
    or the non-`.git` regular file-tree hash changed. The error reports that
    package and configuration changes have already completed.

## Automated host tests

The hermetic fake-SSH/apt/dpkg test suite is registered in CTest:

```sh
ctest --preset host-dev -R '^deploy_ssh_harness$' --output-on-failure
```

## Manual live verification on the Jetson

After building two distinct `arm64` `.deb` artifacts (`pkgA` and `pkgB`) on the
ARM64 builder:

1. **Dry-run check:**
   ```sh
   tools/deploy/aa-deploy.sh zucchi@192.168.55.3 "$pkgA" --confirm-jurisdiction US --dry-run
   ```
2. **Initial install (and legacy migration):**
   ```sh
   tools/deploy/aa-deploy.sh zucchi@192.168.55.3 "$pkgA" --confirm-jurisdiction US
   ```
   Verify on the Jetson:
   - `dpkg-query -W android-auto-receiver` reports `pkgA`'s version.
   - `sudo -n cat /etc/android-auto-receiver/receiver.sha256` matches
     `sha256sum /usr/bin/aa-wireless-receiver | cut -d' ' -f1`.
   - `systemctl status aa-system-helper.service` shows the unit loaded from
     `/usr/lib/systemd/system/aa-system-helper.service`.
   - `/etc/udev/rules.d/70-android-auto-receiver.rules` is absent and
     `/usr/lib/udev/rules.d/70-android-auto-receiver.rules` is present.
3. **Idempotence check:**
   Run the exact same `pkgA` install command a second time and verify `dpkg-query -W`,
   `receiver.sha256`, and `/var/lib/android-auto-receiver/deploy/previous.env` show
   no diff beyond the new run directory.
4. **Upgrade and 60 s rollback timing:**
   ```sh
   tools/deploy/aa-deploy.sh zucchi@192.168.55.3 "$pkgB" --confirm-jurisdiction US
   time tools/deploy/aa-deploy.sh zucchi@192.168.55.3 --rollback
   ```
   Confirm rollback completes in under 60 s, restores `pkgA`'s version and
   `receiver.sha256`, leaves `~/Desktop/infotainment` unchanged, and restores
   projection (see [#191](https://github.com/zucch1/android-auto-jetson/issues/191)).
5. **Post-install sysroot comparison:**
   Re-verify the target package set against the pinned cross-build sysroot manifest
   (`tools/sysroot/cli.py`); if any system library drifted, regenerate the sysroot
   and requalify before release.
