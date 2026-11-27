# Deploying, upgrading, and rolling back `android-auto-receiver` over SSH

`tools/deploy/aa-deploy.sh` (implemented in `tools/deploy/aa_deploy.py`) deploys a
built `arm64` `.deb` package (see [debian-package.md](debian-package.md)) from the
development host to the target Jetson over SSH, replacing the historical ad-hoc
`~/aa-build/deploy-*.sh` script.

## Prerequisites on the target

Before running `aa-deploy.sh`, the operator must already have provisioned:

1. Python 3, `flock` (from util-linux), and passwordless `sudo -n` for the SSH
   user on the target. The tool runs Python 3 as root for file and package
   verification. Deploy, rollback, and adoption share `/run/aa-deploy.lock`.
   If the tool times out after 120 seconds, run
   `sudo fuser -v /run/aa-deploy.lock` on the target and retry once the holder
   finishes.
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
APT or helper restart failures retain the rollback record for a retry.
Rollback is single-level. A successful rollback consumes the previous record.
Keep the legacy backup directory referenced in `previous.env`, typically
`runs/<id>-deploy/legacy` under the state directory, and any archive that a
record references.

## Recovering deployment state

Use adoption if an interrupted install leaves `current.env` missing or a
manual package install leaves it stale. Deploy and any rollback that needs
to remove the package refuse inconsistent current metadata. A rollback to a
recorded Debian archive can proceed using its own verified archive.

On the development host, compute the SHA-256 from the trusted original
`arm64` package for the installed version, or use its published digest.
Set `target` to the original SSH destination and pass `--state-dir` if
you used a custom state directory.

```sh
sha=$(sha256sum "$original_deb" | cut -d' ' -f1)
tools/deploy/aa-deploy.sh "$target" --adopt-installed "$sha"
```

Adoption checks dpkg's installed version, the matching version-keyed archive in
`packages/`, and the installed payload, including file modes. It lists and
rejects unexpected machine-wide systemd overrides for the helper and receiver:
drop-ins in `/etc`, `/run`, `/usr/local/lib`, `/usr/lib` and the
`system.control` directories used by `systemctl set-property`, plus unit files
or mask symlinks that shadow the packaged units, such as a legacy
`/etc/systemd/system/aa-system-helper.service` left by an interrupted migration.
Any host-wide `service.d` drop-in is rejected too, even an unrelated one,
because it applies to every service. Review and resolve those overrides before
retrying. Per-user receiver projection drop-ins are operator-owned.
Successful verification records a dpkg inventory under `runs/<id>-adopt/`
even when `current.env` already matches. Adoption requires a root-owned
mode-0700 state directory and cache.
If `previous.env` exists, review the recorded target before rolling back.
If it is absent, rollback is unavailable until a later deploy records a previous
version. Adoption preserves the backups and any unpublished `previous.env` in
`runs/<id>-deploy/`. If the original migration stopped before removing legacy
shadow files, review those files against the saved backup before resolving them
manually. Then redeploy the identical package with `--confirm-jurisdiction` to
refresh the receiver digest and service configuration.

If the archive is missing or damaged, copy the trusted original package to the
target. Run the following on the target, setting `source_deb` to that local copy
and `sha` to the trusted digest from the development host. Use the same state
directory as the original invocation if it passed `--state-dir`.

```sh
state=/var/lib/android-auto-receiver/deploy
version=$(dpkg-query -W -f='${Version}' android-auto-receiver)
archive="$state/packages/android-auto-receiver_${version}_arm64.deb"
sudo flock --close --timeout 120 /run/aa-deploy.lock \
  bash -es -- "$state" "$archive" "$source_deb" "$sha" <<'RECOVER'
  install -d -m 0700 -o root -g root "$1" "$1/packages"
  temp=$(mktemp -- "$1/packages/.recovery.XXXXXX")
  trap 'rm -f -- "$temp"' EXIT
  install -m 0644 -o root -g root -- "$3" "$temp"
  actual=$(sha256sum -- "$temp")
  if [ "${actual%% *}" != "$4" ]; then
    echo "recovery: source package SHA-256 mismatch" >&2
    exit 1
  fi
  mv -fT -- "$temp" "$2"
RECOVER
```

Retry adoption with the same trusted digest. If it reports altered or missing
installed files, reinstall the verified original on the target with
`sudo dpkg -i -- "$archive"`. Resolve any reported dependency or configuration
errors and retry adoption. If dpkg reports an unfinished configuration, run
`sudo dpkg --configure -a` and resolve any errors first. Perform manual
package-manager recovery with other deploy operations stopped.

If the current archive is missing or corrupt and `current.env` still records
the installed version and trusted digest, redeploy the identical package to
restore the cache and retain the previous record. An upgrade needs the current archive as a rollback
artifact. If a rollback's previous archive is missing or corrupt, use the
recorded `DEB_PATH` and `DEB_SHA256` from `previous.env` for the cache restore
command above, then retry rollback.

If dpkg already removed the package during rollback, retry `--rollback` to
finish restoration before deploying again. If no usable previous record
survives, complete any required legacy restoration from the saved run directory
and move stale `current.env` or `previous.env` out of the state directory for
reference before a fresh deploy.

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
   [adoption](#recovering-deployment-state) before deploy can proceed. Cached archives are immutable per
   Debian version. Build with a new version when the bytes change. Reinstalling the
   exact same package preserves `previous.env`.
7. **Legacy ad-hoc migration:** backs up the artifacts below in the deploy run's
   `legacy/` directory before invoking APT. Backup errors and symlinks at
   the legacy paths abort installation. Their targets may live outside the
   backup, so resolve the layout manually before migration.
   After installation succeeds, deploy publishes the rollback record and removes
   the shadowing config:
   - `/etc/udev/rules.d/70-android-auto-receiver.rules` (shadows the packaged
     `/usr/lib/udev/rules.d/70-android-auto-receiver.rules`).
   - `/etc/systemd/system/aa-system-helper.service` (shadows the packaged
     `/usr/lib/systemd/system/aa-system-helper.service`). For a unit with
     `ExecStart=/usr/libexec/aa-system-helper`, deploy also saves the original
     executable and rollback restores it before restoring the unit. Other units
     need one literal absolute `ExecStart` path and its executable must remain
     available. Deploy refuses a missing or non-executable target because it
     could not start the restored service. Custom
     executables must survive package removal. Deploy refuses an executable
     whose path or resolved target overlaps another file in the incoming package.
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
