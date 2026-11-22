#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hermetic fake-SSH/apt/dpkg tests for tools/deploy/aa-deploy.sh."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
DEPLOY_SH = ROOT / "tools" / "deploy" / "aa-deploy.sh"

FAKE_SSH = r"""#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_SSH_LOG:-}" ]; then
    printf 'ssh %s\n' "$*" >> "$FAKE_SSH_LOG"
fi
cmd="${@: -1}"
root="${FAKE_TARGET_ROOT:?}"
# Rewrite target system paths into the hermetic fake target root.
cmd="${cmd//\/etc\/os-release/$root\/etc\/os-release}"
cmd="${cmd//\/etc\/android-auto-receiver/$root\/etc\/android-auto-receiver}"
cmd="${cmd//\/etc\/udev\/rules.d/$root\/etc\/udev\/rules.d}"
cmd="${cmd//\/etc\/systemd\/system/$root\/etc\/systemd\/system}"
cmd="${cmd//\/etc\/dbus-1\/system.d/$root\/etc\/dbus-1\/system.d}"
cmd="${cmd//\/usr\/bin\/aa-wireless-receiver/$root\/usr\/bin\/aa-wireless-receiver}"
cmd="${cmd//\/var\/lib\/android-auto-receiver/$root\/var\/lib\/android-auto-receiver}"
cmd="${cmd//\/tmp\/aa-deploy-/$root\/tmp\/aa-deploy-}"
HOME="$root/home/zucchi" PATH="$FAKE_REMOTE_BIN:$PATH" bash -euc "$cmd"
"""

FAKE_SCP = r"""#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_SSH_LOG:-}" ]; then
    printf 'scp %s\n' "$*" >> "$FAKE_SSH_LOG"
fi
root="${FAKE_TARGET_ROOT:?}"
src="${@: -2:1}"
dst="${@: -1}"
remote_path="${dst#*:}"
remote_path="${remote_path/\/tmp\/aa-deploy-/$root\/tmp\/aa-deploy-}"
mkdir -p "$(dirname -- "$remote_path")"
cp -p "$src" "$remote_path"
"""

FAKE_SUDO = r"""#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" != "-n" ]; then
    echo "fake-sudo: refusing sudo without -n" >&2
    exit 99
fi
shift
exec "$@"
"""

FAKE_INSTALL = r"""#!/usr/bin/env bash
set -euo pipefail
args=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        -o|-g) shift 2 ;;
        *) args+=("$1"); shift ;;
    esac
done
exec /usr/bin/install "${args[@]}"
"""

FAKE_CHOWN = r"""#!/usr/bin/env bash
set -euo pipefail
exit 0
"""

FAKE_DPKG = r"""#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = "--print-architecture" ]; then
    printf '%s\n' "${FAKE_ARCH:-arm64}"
    exit 0
fi
exec /usr/bin/dpkg "$@"
"""

FAKE_UNAME = r"""#!/usr/bin/env bash
set -euo pipefail
if [ "${1:-}" = "-m" ]; then
    printf '%s\n' "${FAKE_UNAME_M:-aarch64}"
    exit 0
fi
exec /usr/bin/uname "$@"
"""

FAKE_DF = r"""#!/usr/bin/env bash
set -euo pipefail
printf 'Filesystem 1048576-blocks Used Available Capacity Mounted on\n'
printf '/dev/fake 100000 50000 %s 50%% /\n' "${FAKE_FREE_MB:-4096}"
"""

FAKE_DPKG_QUERY = r"""#!/usr/bin/env bash
set -euo pipefail
status_file="${FAKE_TARGET_ROOT:?}/var/lib/dpkg/fake-packages.txt"
if [ -n "${FAKE_EVENT_LOG:-}" ]; then
    printf 'dpkg-query %s\n' "$*" >> "$FAKE_EVENT_LOG"
fi
if [ "${1:-}" = "-W" ] && [ "$#" -eq 1 ]; then
    cat "$status_file"
    exit 0
fi
exec /usr/bin/dpkg-query "$@"
"""

FAKE_APT_GET = r"""#!/usr/bin/env bash
set -euo pipefail
root="${FAKE_TARGET_ROOT:?}"
status_file="$root/var/lib/dpkg/fake-packages.txt"
if [ -n "${FAKE_EVENT_LOG:-}" ]; then
    printf 'apt-get %s\n' "$*" >> "$FAKE_EVENT_LOG"
fi
# Assert that dpkg-query -W was recorded BEFORE any apt-get invocation.
if ! find "$root/var/lib/android-auto-receiver/deploy/runs" -name 'dpkg-query-before.txt' -size +0c 2>/dev/null | grep -q .; then
    echo "fake-apt-get: dpkg-query-before.txt missing before apt-get $*" >&2
    exit 97
fi
if [ "${1:-}" = "-s" ] && [ "${2:-}" = "install" ]; then
    if [ "${FAKE_DEPENDS_FAIL:-0}" = "1" ]; then
        echo "E: Unable to correct problems, you have held broken packages." >&2
        exit 100
    fi
    exit 0
fi
if [ "${1:-}" = "install" ]; then
    deb="${@: -1}"
    ver="$(/usr/bin/dpkg-deb --field "$deb" Version)"
    /usr/bin/dpkg-deb --extract "$deb" "$root"
    grep -v '^android-auto-receiver[[:space:]]' "$status_file" > "$status_file.new" || true
    printf 'android-auto-receiver\t%s\n' "$ver" >> "$status_file.new"
    mv -f "$status_file.new" "$status_file"
    if [ "${FAKE_TAMPER_INFOTAINMENT:-0}" = "1" ]; then
        printf 'tampered\n' >> "$root/home/zucchi/Desktop/infotainment/README.md"
    fi
    exit 0
fi
if [ "${1:-}" = "remove" ]; then
    grep -v '^android-auto-receiver[[:space:]]' "$status_file" > "$status_file.new" || true
    mv -f "$status_file.new" "$status_file"
    rm -f "$root/usr/bin/aa-wireless-receiver"
    exit 0
fi
echo "fake-apt-get: unexpected args: $*" >&2
exit 96
"""

FAKE_SYSTEMCTL = r"""#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_EVENT_LOG:-}" ]; then
    printf 'systemctl %s\n' "$*" >> "$FAKE_EVENT_LOG"
fi
case "${1:-}" in
    enable|start)
        echo "fake-systemctl: forbidden command $*" >&2
        exit 95
        ;;
    is-active)
        state="${FAKE_HELPER_ACTIVE:-inactive}"
        printf '%s\n' "$state"
        [ "$state" = "active" ] && exit 0 || exit 3
        ;;
    is-enabled)
        state="${FAKE_HELPER_ENABLED:-disabled}"
        printf '%s\n' "$state"
        [ "$state" = "enabled" ] && exit 0 || exit 1
        ;;
    daemon-reload|restart)
        exit 0
        ;;
    *)
        echo "fake-systemctl: unexpected args: $*" >&2
        exit 94
        ;;
esac
"""

FAKE_UDEVADM = r"""#!/usr/bin/env bash
set -euo pipefail
if [ -n "${FAKE_EVENT_LOG:-}" ]; then
    printf 'udevadm %s\n' "$*" >> "$FAKE_EVENT_LOG"
fi
exit 0
"""


def write_exec(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(0o755)


def make_deb(directory: Path, version: str, receiver_payload: bytes, arch: str = "arm64") -> Path:
    pkg_dir = directory / f"pkg-{version}"
    (pkg_dir / "DEBIAN").mkdir(parents=True)
    (pkg_dir / "usr/bin").mkdir(parents=True)
    (pkg_dir / "usr/lib/systemd/system").mkdir(parents=True)
    (pkg_dir / "usr/lib/udev/rules.d").mkdir(parents=True)
    (pkg_dir / "usr/share/dbus-1/system.d").mkdir(parents=True)
    (pkg_dir / "DEBIAN/control").write_text(
        f"Package: android-auto-receiver\n"
        f"Version: {version}\n"
        f"Architecture: {arch}\n"
        f"Maintainer: Test <test@example.invalid>\n"
        f"Depends: libc6\n"
        f"Description: Test fixture\n"
    )
    receiver = pkg_dir / "usr/bin/aa-wireless-receiver"
    receiver.write_bytes(receiver_payload)
    receiver.chmod(0o755)
    (pkg_dir / "usr/lib/systemd/system/aa-system-helper.service").write_text("[Unit]\nDescription=packaged\n")
    (pkg_dir / "usr/lib/udev/rules.d/70-android-auto-receiver.rules").write_text("# packaged udev\n")
    (pkg_dir / "usr/share/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf").write_text("<busconfig/>\n")
    deb_path = directory / f"android-auto-receiver_{version}_{arch}.deb"
    subprocess.run(["dpkg-deb", "--build", str(pkg_dir), str(deb_path)], check=True, capture_output=True)
    return deb_path


class DeployHarnessTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="aa-deploy-test-")
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.host_bin = self.base / "host-bin"
        self.remote_bin = self.base / "remote-bin"
        self.target = self.base / "target"
        self.host_bin.mkdir()
        self.remote_bin.mkdir()

        write_exec(self.host_bin / "ssh", FAKE_SSH)
        write_exec(self.host_bin / "scp", FAKE_SCP)
        write_exec(self.remote_bin / "sudo", FAKE_SUDO)
        write_exec(self.remote_bin / "install", FAKE_INSTALL)
        write_exec(self.remote_bin / "chown", FAKE_CHOWN)
        write_exec(self.remote_bin / "dpkg", FAKE_DPKG)
        write_exec(self.remote_bin / "dpkg-query", FAKE_DPKG_QUERY)
        write_exec(self.remote_bin / "apt-get", FAKE_APT_GET)
        write_exec(self.remote_bin / "systemctl", FAKE_SYSTEMCTL)
        write_exec(self.remote_bin / "udevadm", FAKE_UDEVADM)
        write_exec(self.remote_bin / "df", FAKE_DF)
        write_exec(self.remote_bin / "uname", FAKE_UNAME)

        # Populate fake target filesystem.
        for rel in (
            "etc/android-auto-receiver",
            "etc/udev/rules.d",
            "etc/systemd/system",
            "etc/dbus-1/system.d",
            "usr/bin",
            "var/lib/dpkg",
            "var/lib/android-auto-receiver",
            "tmp",
            "home/zucchi/Desktop/infotainment",
        ):
            (self.target / rel).mkdir(parents=True, exist_ok=True)

        (self.target / "etc/os-release").write_text('ID=ubuntu\nVERSION_ID="24.04"\n')
        ap_conf = self.target / "etc/android-auto-receiver/ap.conf"
        ap_conf.write_text("v1 wlan0 US jurisdiction-confirmed 5277 44\n")
        ap_conf.chmod(0o600)
        session_uid = self.target / "etc/android-auto-receiver/session.uid"
        session_uid.write_text("2002\n")
        session_uid.chmod(0o600)
        (self.target / "var/lib/dpkg/fake-packages.txt").write_text("base-files\t13ubuntu10\nlibc6\t2.39-0ubuntu8\n")

        # Real git repo for ~/Desktop/infotainment (including a pre-existing dirty edit like the Jetson).
        info_dir = self.target / "home/zucchi/Desktop/infotainment"
        (info_dir / "README.md").write_text("infotainment baseline\n")
        subprocess.run(["git", "-C", str(info_dir), "init", "-q"], check=True)
        subprocess.run(["git", "-C", str(info_dir), "config", "user.name", "Test"], check=True)
        subprocess.run(["git", "-C", str(info_dir), "config", "user.email", "test@example.invalid"], check=True)
        subprocess.run(["git", "-C", str(info_dir), "add", "README.md"], check=True)
        subprocess.run(["git", "-C", str(info_dir), "commit", "-q", "-m", "init"], check=True)
        (info_dir / "README.md").write_text("infotainment baseline + existing edit\n")

        self.pkg_v1 = make_deb(self.base, "1.0.0", b"ELF-RECEIVER-V1\n")
        self.pkg_v2 = make_deb(self.base, "1.1.0", b"ELF-RECEIVER-V2\n")
        self.ssh_log = self.base / "ssh.log"
        self.event_log = self.base / "events.log"

    def run_deploy(self, *args: str, extra_env: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
        env = {
            **os.environ,
            "PATH": f"{self.host_bin}:{os.environ['PATH']}",
            "FAKE_TARGET_ROOT": str(self.target),
            "FAKE_REMOTE_BIN": str(self.remote_bin),
            "FAKE_SSH_LOG": str(self.ssh_log),
            "FAKE_EVENT_LOG": str(self.event_log),
            **(extra_env or {}),
        }
        return subprocess.run(
            [str(DEPLOY_SH), *args],
            text=True,
            capture_output=True,
            env=env,
            check=False,
        )

    def test_preflight_refusals_and_no_secret_leak(self) -> None:
        # Missing --confirm-jurisdiction
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1))
        self.assertNotEqual(res.returncode, 0)
        self.assertFalse(self.ssh_log.exists())

        # Measurement in progress
        marker = self.target / "home/zucchi/MEASUREMENT_IN_PROGRESS"
        marker.write_text("active\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("measurement in progress", res.stderr)
        marker.unlink()

        # Wrong architecture
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_ARCH": "amd64"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("unsupported dpkg architecture", res.stderr)

        # Wrong OS release
        (self.target / "etc/os-release").write_text('ID=ubuntu\nVERSION_ID="22.04"\n')
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("unsupported OS", res.stderr)
        (self.target / "etc/os-release").write_text('ID=ubuntu\nVERSION_ID="24.04"\n')

        # Insufficient free disk
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_FREE_MB": "100"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("insufficient free disk", res.stderr)

        # Unconfirmed jurisdiction in ap.conf (and verify ap.conf contents never leak)
        ap_conf = self.target / "etc/android-auto-receiver/ap.conf"
        ap_conf.write_text("v1 wlan0 US jurisdiction-unconfirmed 5277 44\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("jurisdiction-confirmed", res.stderr)
        self.assertNotIn("wlan0", res.stdout + res.stderr)
        self.assertNotIn("5277", res.stdout + res.stderr)

        # Jurisdiction mismatch
        ap_conf.write_text("v1 wlan0 DE jurisdiction-confirmed 5277 44\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("does not match", res.stderr)
        self.assertNotIn("DE", res.stdout + res.stderr)
        ap_conf.write_text("v1 wlan0 US jurisdiction-confirmed 5277 44\n")

        # Unsatisfiable Depends: dpkg-query -W recorded before apt-get -s, and no install occurs
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_DEPENDS_FAIL": "1"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("Depends not satisfiable", res.stderr)
        self.assertFalse((self.target / "usr/bin/aa-wireless-receiver").exists())

    def test_install_migration_digest_from_installed_and_no_enable_inactive_helper(self) -> None:
        # Pre-populate legacy ad-hoc install artifacts
        legacy_bin_dir = self.target / "home/zucchi/aa-build/bin"
        legacy_bin_dir.mkdir(parents=True)
        legacy_recv = legacy_bin_dir / "aa-wireless-receiver"
        legacy_recv.write_bytes(b"LEGACY-RECEIVER-DIFFERENT-BYTES\n")
        legacy_recv.chmod(0o755)
        legacy_udev = self.target / "etc/udev/rules.d/70-android-auto-receiver.rules"
        legacy_udev.write_text("# legacy udev\n")
        legacy_unit = self.target / "etc/systemd/system/aa-system-helper.service"
        legacy_unit.write_text("[Unit]\nDescription=legacy\n")
        legacy_dbus = self.target / "etc/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf"
        legacy_dbus.write_text("<busconfig><!-- legacy --></busconfig>\n")
        allow_frag = self.target / "etc/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.receiver-2002.conf"
        allow_frag.write_text("<busconfig><!-- keep --></busconfig>\n")
        legacy_sha = self.target / "etc/android-auto-receiver/receiver.sha256"
        legacy_sha.write_text("0" * 64 + "\n")

        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_HELPER_ACTIVE": "inactive", "FAKE_HELPER_ENABLED": "disabled"},
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        # Never leak ap.conf contents
        self.assertNotIn("wlan0", res.stdout + res.stderr)
        self.assertNotIn("5277", res.stdout + res.stderr)

        # Legacy shadowing files migrated; per-user D-Bus allow fragment preserved
        self.assertFalse(legacy_udev.exists())
        self.assertFalse(legacy_unit.exists())
        self.assertFalse(legacy_dbus.exists())
        self.assertTrue(allow_frag.exists())

        # Digest computed from INSTALLED receiver, not legacy binary
        expected_digest = hashlib.sha256(b"ELF-RECEIVER-V1\n").hexdigest()
        legacy_digest = hashlib.sha256(b"LEGACY-RECEIVER-DIFFERENT-BYTES\n").hexdigest()
        self.assertNotEqual(expected_digest, legacy_digest)
        self.assertEqual(legacy_sha.read_text(), expected_digest + "\n")

        # Package set recorded before apt-get
        events = self.event_log.read_text().splitlines()
        first_dpkg_q = next(i for i, e in enumerate(events) if e.startswith("dpkg-query "))
        first_apt = next(i for i, e in enumerate(events) if e.startswith("apt-get "))
        self.assertLess(first_dpkg_q, first_apt)

        # Inactive & disabled helper was NOT restarted, started, or enabled
        self.assertFalse(any(e.startswith("systemctl restart") for e in events), events)
        self.assertFalse(any(e.startswith("systemctl enable") for e in events), events)
        self.assertFalse(any(e.startswith("systemctl start") for e in events), events)

        # Rollback back to legacy restores legacy files and digest
        rb = self.run_deploy(
            "zucchi@jetson", "--rollback",
            extra_env={"FAKE_HELPER_ACTIVE": "inactive", "FAKE_HELPER_ENABLED": "disabled"},
        )
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertTrue(legacy_udev.exists())
        self.assertTrue(legacy_unit.exists())
        self.assertTrue(legacy_dbus.exists())
        self.assertEqual(legacy_sha.read_text(), "0" * 64 + "\n")

    def test_idempotence_upgrade_and_deb_rollback_with_active_helper(self) -> None:
        env = {"FAKE_HELPER_ACTIVE": "active", "FAKE_HELPER_ENABLED": "enabled"}
        res1 = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US", extra_env=env)
        self.assertEqual(res1.returncode, 0, res1.stderr)
        res2 = self.run_deploy("zucchi@jetson", str(self.pkg_v2), "--confirm-jurisdiction", "US", extra_env=env)
        self.assertEqual(res2.returncode, 0, res2.stderr)

        v1_digest = hashlib.sha256(b"ELF-RECEIVER-V1\n").hexdigest()
        v2_digest = hashlib.sha256(b"ELF-RECEIVER-V2\n").hexdigest()
        sha_file = self.target / "etc/android-auto-receiver/receiver.sha256"
        prev_env = self.target / "var/lib/android-auto-receiver/deploy/previous.env"
        pkgs_file = self.target / "var/lib/dpkg/fake-packages.txt"

        self.assertEqual(sha_file.read_text(), v2_digest + "\n")
        self.assertIn("VERSION=1.0.0", prev_env.read_text())

        # Idempotent reinstall of v2 produces no diff in package/digest and keeps previous.env at v1
        before_pkgs = pkgs_file.read_text()
        before_prev = prev_env.read_text()
        res3 = self.run_deploy("zucchi@jetson", str(self.pkg_v2), "--confirm-jurisdiction", "US", extra_env=env)
        self.assertEqual(res3.returncode, 0, res3.stderr)
        self.assertEqual(pkgs_file.read_text(), before_pkgs)
        self.assertEqual(sha_file.read_text(), v2_digest + "\n")
        self.assertEqual(prev_env.read_text(), before_prev)

        # Helper was restarted (was active) and never enabled
        events = self.event_log.read_text().splitlines()
        self.assertTrue(any(e == "systemctl restart aa-system-helper.service" for e in events))
        self.assertFalse(any(e.startswith("systemctl enable") for e in events))

        # Rollback restores v1 package and v1 digest
        rb = self.run_deploy("zucchi@jetson", "--rollback", extra_env=env)
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertIn("android-auto-receiver\t1.0.0", pkgs_file.read_text())
        self.assertEqual(sha_file.read_text(), v1_digest + "\n")
        self.assertEqual((self.target / "usr/bin/aa-wireless-receiver").read_bytes(), b"ELF-RECEIVER-V1\n")

    def test_infotainment_modified_fails(self) -> None:
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_TAMPER_INFOTAINMENT": "1"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("infotainment tree modified", res.stderr)

    def test_dry_run_prints_commands_without_running_ssh(self) -> None:
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US", "--dry-run")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("[dry-run] ssh", res.stdout)
        self.assertIn("[dry-run] scp", res.stdout)
        self.assertIn("apt-get install", res.stdout)
        self.assertFalse(self.ssh_log.exists())
        self.assertFalse(self.event_log.exists())

        rb = self.run_deploy("zucchi@jetson", "--rollback", "--dry-run")
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertIn("[dry-run] ssh", rb.stdout)
        self.assertIn("--allow-downgrades", rb.stdout)
        self.assertFalse(self.ssh_log.exists())


if __name__ == "__main__":
    unittest.main()
