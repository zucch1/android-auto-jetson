#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hermetic fake-SSH/apt/dpkg tests for tools/deploy/aa-deploy.sh."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
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
root="${FAKE_TARGET_ROOT:?}"
if [ "$#" -ge 2 ] && [ "${@: -2:1}" = "bash" ] && [ "${@: -1}" = "-s" ]; then
    raw_cmd="$(cat)"
else
    raw_cmd="${@: -1}"
fi
# Single-pass, unambiguous path rewrite into the hermetic fake target root.
# Matching an existing $root/... prefix first prevents double-rewriting even when
# TMPDIR is unset or /tmp (so $root itself begins with /tmp/aa-deploy-test-...).
cmd="$(python3 -c '
import os, re, sys
root = os.environ["FAKE_TARGET_ROOT"]
src = sys.stdin.read()
pat = re.compile(
    re.escape(root) + r"[^\s\x27\";]*"
    + r"|(?:/etc/os-release"
    + r"|/etc/android-auto-receiver"
    + r"|/etc/udev/rules\.d"
    + r"|/etc/systemd/system"
    + r"|/etc/dbus-1/system\.d"
    + r"|/usr/bin/aa-wireless-receiver"
    + r"|/usr/libexec/aa-system-helper"
    + r"|/run/aa-deploy.lock"
    + r"|/var/lib/android-auto-receiver"
    + r"|/tmp/aa-deploy-)"
)
def repl(m: re.Match[str]) -> str:
    val = m.group(0)
    if val.startswith(root):
        return val
    return root + val
sys.stdout.write(pat.sub(repl, src))
' <<< "$raw_cmd")"
cd "$root/home/zucchi"
HOME="$root/home/zucchi" PATH="$FAKE_REMOTE_BIN:$PATH" bash -euo pipefail -s <<< "$cmd"
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
case "$remote_path" in
    "$root"/*) ;;
    /tmp/aa-deploy-*) remote_path="${root}${remote_path}" ;;
esac
mkdir -p "$(dirname -- "$remote_path")"
cp -p "$src" "$remote_path"
if [ "${FAKE_CORRUPT_SCP:-0}" = "1" ]; then
    printf 'X' >> "$remote_path"
fi
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
if [ "${FAKE_METADATA_BACKUP_FAIL:-0}" = "1" ] &&
    [[ "${@: -2:1}" == */deploy/current.env && "${@: -1}" == */runs/*/previous.env ]]; then
    echo "fake-install: metadata backup failed" >&2
    exit 1
fi
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

# Run the actual remote validator with the fake target as its root and inject
# ownership only. File descriptors, modes, links, and bytes remain real.
FAKE_REMOTE_PYTHON = r"""#!/usr/bin/python3
import os
import sys

if sys.argv[1:3] == ["-I", "-c"] and sys.argv[4:] == ["/"]:
    os.execv("/usr/bin/python3", ["python3", *sys.argv[1:4], os.environ["FAKE_TARGET_ROOT"]])
if sys.argv[1:4] != ["-I", "-", "/"]:
    os.execv("/usr/bin/python3", ["python3", *sys.argv[1:]])
sys.argv = ["-", os.environ["FAKE_TARGET_ROOT"], *sys.argv[4:]]
real_fstat = os.fstat

def fake_fstat(fd):
    result = list(real_fstat(fd))
    path = os.readlink(f"/proc/self/fd/{fd}")
    result[4] = int(os.environ.get("FAKE_AP_UID", "0")) if path.endswith("/ap.conf") else 0
    return os.stat_result(result)

os.fstat = fake_fstat
exec(compile(sys.stdin.read(), "<remote validator>", "exec"))
"""


FAKE_FLOCK = r"""#!/usr/bin/env bash
set -euo pipefail
if [ "${FAKE_LOCK_TIMEOUT:-0}" = "1" ]; then
    [ "$1 $2 $3 $4 $5" = "--close --timeout 120 --conflict-exit-code 75" ]
    exit 75
fi
exec /usr/bin/flock "$@"
"""

FAKE_MV = r"""#!/usr/bin/env bash
set -euo pipefail
if [ "${FAKE_INTERRUPT_RECORD_PUBLISH:-0}" = "1" ] &&
    [[ "${@: -1}" == */deploy/current.env ]]; then
    echo "fake-mv: interrupted before publishing current.env" >&2
    exit 1
fi
exec /usr/bin/mv "$@"
"""

FAKE_CP = r"""#!/usr/bin/env bash
set -euo pipefail
if [ "${FAKE_LEGACY_BACKUP_FAIL:-0}" = "1" ] && [[ "$*" == *'/etc/udev/rules.d/'* ]]; then
    echo "fake-cp: legacy backup failed" >&2
    exit 1
fi
exec /usr/bin/cp "$@"
"""

FAKE_SHA256SUM = r"""#!/usr/bin/env bash
set -euo pipefail
for arg; do
    if [[ "$arg" == *'/unreadable-fixture' ]]; then
        echo "sha256sum: unreadable-fixture: Permission denied" >&2
        exit 1
    fi
done
exec /usr/bin/sha256sum "$@"
"""

FAKE_GIT = r"""#!/usr/bin/env bash
set -euo pipefail
printf 'git %s\n' "$*" >> "${FAKE_EVENT_LOG:?}"
exec /usr/bin/git "$@"
"""

FAKE_STAT = r"""#!/usr/bin/env bash
set -euo pipefail
out="$(/usr/bin/stat "$@")"
if [ -n "${FAKE_STAT_UID:-}" ] && [ "${1:-}" = "-c" ] && [[ "${2:-}" == %u* ]]; then
    printf '%s%s\n' "$FAKE_STAT_UID" "${out#${out%% *}}"
    exit 0
fi
printf '%s\n' "$out"
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
if [ "${1:-}" = "-W" ] && [ "${@: -1}" = "android-auto-receiver" ]; then
    awk '$1 == "android-auto-receiver" {print "installed", $2; found=1} END {exit !found}' "$status_file"
    exit $?
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
if [ "${1:-}" = "-s" ]; then
    if [ "${FAKE_DEPENDS_FAIL:-0}" = "1" ]; then
        echo "E: Unable to correct problems, you have held broken packages." >&2
        exit 100
    fi
    exit 0
fi
if [ "${1:-}" = "install" ]; then
    if [ "${FAKE_INSTALL_FAIL:-0}" = "1" ]; then
        echo "E: Sub-process /usr/bin/dpkg returned an error code (1)" >&2
        exit 100
    fi
    deb="${@: -1}"
    ver="$(/usr/bin/dpkg-deb --field "$deb" Version)"
    # Real APT semantics: an already-installed identical version is a no-op without --reinstall.
    reinstall=0
    for arg in "$@"; do [ "$arg" = "--reinstall" ] && reinstall=1; done
    if [ "$reinstall" = 0 ] && grep -qx "android-auto-receiver	$ver" "$status_file" 2>/dev/null; then
        echo "android-auto-receiver is already the newest version ($ver)."
        exit 0
    fi
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
    if [ "${FAKE_REMOVE_FAIL:-0}" = "1" ]; then
        echo "fake-apt-get: removal failed" >&2
        exit 100
    fi
    if [ "${FAKE_REMOVE_NOOP:-0}" = "1" ]; then
        exit 0
    fi
    grep -v '^android-auto-receiver[[:space:]]' "$status_file" > "$status_file.new" || true
    mv -f "$status_file.new" "$status_file"
    rm -f "$root/usr/bin/aa-wireless-receiver" "$root/usr/libexec/aa-system-helper"
    rmdir --ignore-fail-on-non-empty "$root/usr/libexec"
    if [ "${FAKE_TAMPER_INFOTAINMENT:-0}" = "1" ]; then
        printf 'tampered\n' >> "$root/home/zucchi/Desktop/infotainment/README.md"
    fi
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
        if [ "$1" = restart ] && [ "${FAKE_RESTART_FAIL:-0}" = "1" ]; then
            echo "fake-systemctl: restart failed" >&2
            exit 1
        fi
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
    (pkg_dir / "usr/libexec").mkdir(parents=True)
    write_exec(pkg_dir / "usr/libexec/aa-system-helper", "#!/bin/sh\n# packaged helper\n")
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
        write_exec(self.remote_bin / "stat", FAKE_STAT)
        write_exec(self.remote_bin / "flock", FAKE_FLOCK)
        write_exec(self.remote_bin / "mv", FAKE_MV)
        write_exec(self.remote_bin / "python3", FAKE_REMOTE_PYTHON)
        write_exec(self.remote_bin / "cp", FAKE_CP)
        write_exec(self.remote_bin / "git", FAKE_GIT)
        write_exec(self.remote_bin / "sha256sum", FAKE_SHA256SUM)
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
            "usr/libexec",
            "run",
            "var/lib/dpkg",
            "var/lib/android-auto-receiver",
            "tmp",
            "home/zucchi/Desktop/infotainment",
        ):
            (self.target / rel).mkdir(parents=True, exist_ok=True)
        (self.target / "etc").chmod(0o755)
        (self.target / "etc/android-auto-receiver").chmod(0o755)

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
            "FAKE_STAT_UID": "0",
            **(extra_env or {}),
        }
        return subprocess.run(
            [str(DEPLOY_SH), *args],
            text=True,
            capture_output=True,
            env=env,
            check=False,
        )

    def install(self, deb: Path | None = None, **env: str) -> subprocess.CompletedProcess[str]:
        return self.run_deploy(
            "zucchi@jetson", str(deb or self.pkg_v1), "--confirm-jurisdiction", "US", extra_env=env,
        )

    def rollback(self, **env: str) -> subprocess.CompletedProcess[str]:
        return self.run_deploy("zucchi@jetson", "--rollback", extra_env=env)

    def adopt(self, deb: Path, **env: str) -> subprocess.CompletedProcess[str]:
        digest = hashlib.sha256(deb.read_bytes()).hexdigest()
        return self.run_deploy("zucchi@jetson", "--adopt-installed", digest, extra_env=env)

    def legacy_helper(self) -> tuple[Path, Path]:
        helper = self.target / "usr/libexec/aa-system-helper"
        unit = self.target / "etc/systemd/system/aa-system-helper.service"
        helper.parent.mkdir(exist_ok=True)
        write_exec(helper, "#!/bin/sh\n# legacy helper\n")
        unit.write_text("[Service]\nExecStart=/usr/libexec/aa-system-helper\n")
        return helper, unit

    def test_session_uid_is_validated_byte_for_byte(self) -> None:
        uid = self.target / "etc/android-auto-receiver/session.uid"
        for value in (b"2002\n\n", b"20\x0002\n", b"2002" + b"\n" * 125):
            with self.subTest(value=value):
                uid.write_bytes(value)
                result = self.install()
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("session.uid", result.stderr)
        for value in (b"0" * 123 + b"2002\n", b"4294967295"):
            with self.subTest(value=value):
                uid.write_bytes(value)
                result = self.install()
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_session_uid_accepts_world_readable_file(self) -> None:
        uid = self.target / "etc/android-auto-receiver/session.uid"
        uid.chmod(0o644)
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_preflight_rejects_writable_and_symlinked_etc(self) -> None:
        etc = self.target / "etc"
        etc.chmod(0o775)
        result = self.install()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("config dir", result.stderr)
        etc.chmod(0o755)
        etc.rename(self.target / "etc-real")
        etc.symlink_to("etc-real")
        result = self.install()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("config dir", result.stderr)

    def test_ap_policy_validates_raw_tokens_and_bounded_port(self) -> None:
        ap = self.target / "etc/android-auto-receiver/ap.conf"
        for value in (
            b"v1 wl\x00an0 US jurisdiction-confirmed 5277 44\n",
            b"v1 wlan0 US jurisdiction-confirmed 18446744073709556893 44\n",
        ):
            with self.subTest(value=value):
                ap.write_bytes(value)
                result = self.install()
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("ap.conf", result.stderr)
                self.assertNotIn("18446744073709556893", result.stderr)
        ap.write_bytes(b"v1\twlan0 US\r\njurisdiction-confirmed 0005277 00044\n")
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_ap_policy_rejects_interface_glob_even_with_a_match(self) -> None:
        (self.target / "home/zucchi/wlan0").touch()
        (self.target / "etc/android-auto-receiver/ap.conf").write_text(
            "v1 wl* US jurisdiction-confirmed 5277 44\n",
        )
        result = self.install()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("interface name invalid", result.stderr)
        self.assertNotIn("wl*", result.stderr)

    def test_rebuilt_version_cannot_replace_current_or_previous_archive(self) -> None:
        rebuilt = make_deb(self.base / "rebuilt", "1.0.0", b"REBUILT\n")
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        archive = self.target / "var/lib/android-auto-receiver/deploy/packages" / self.pkg_v1.name
        for current in (self.pkg_v1, self.pkg_v2):
            result = self.install(current)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = self.install(rebuilt)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("different bytes", result.stderr)
            self.assertEqual(archive.read_bytes(), self.pkg_v1.read_bytes())
        result = self.rollback()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("rolled_back_to=1.0.0", result.stdout)

    def test_missing_or_inconsistent_metadata_refuses_deploy_and_removal(self) -> None:
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        current = self.target / "var/lib/android-auto-receiver/deploy/current.env"
        saved = current.read_bytes()
        for metadata in (None, saved.replace(b"VERSION=1.0.0", b"VERSION=9.0.0")):
            with self.subTest(metadata=metadata):
                current.unlink(missing_ok=True)
                if metadata is not None:
                    current.write_bytes(metadata)
                for result in (self.install(self.pkg_v2), self.rollback()):
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn("deployment metadata", result.stderr)
        self.assertEqual((self.target / "usr/bin/aa-wireless-receiver").read_bytes(), b"ELF-RECEIVER-V1\n")
        self.assertFalse(any(line.startswith("apt-get remove") for line in self.event_log.read_text().splitlines()))

    def test_adopt_recovers_after_install_interrupted_before_record_publish(self) -> None:
        helper, unit = self.legacy_helper()
        legacy_helper = helper.read_bytes()
        result = self.install(FAKE_INTERRUPT_RECORD_PUBLISH="1")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("interrupted before publishing", result.stderr)
        state = self.target / "var/lib/android-auto-receiver/deploy"
        current = state / "current.env"
        previous = state / "previous.env"
        self.assertFalse(current.exists())
        self.assertFalse(previous.exists())
        self.assertTrue(unit.exists())
        backup, = state.glob("runs/*-deploy/legacy/legacy-helper")
        self.assertEqual(backup.read_bytes(), legacy_helper)

        result = self.install()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("--adopt-installed", result.stderr)
        result = self.rollback()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("no previous deployment recorded", result.stderr)

        archive = state / "packages" / self.pkg_v1.name
        original = archive.read_bytes()
        archive.unlink()
        result = self.adopt(self.pkg_v1)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("missing or non-regular cached archive", result.stderr)
        self.assertIn(self.pkg_v1.name, result.stderr)
        self.assertFalse(current.exists())

        archive.write_bytes(original + b"corrupt")
        result = self.adopt(self.pkg_v1)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("cached archive SHA-256 mismatch", result.stderr)
        self.assertFalse(current.exists())

        archive.write_bytes(original)
        before = self.event_log.read_text()
        result = self.adopt(self.pkg_v1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("recovered current.env for installed version 1.0.0", result.stdout)
        self.assertIn("receiver.sha256 is missing or does not match", result.stdout)
        self.assertIn(f"DEB_PATH={archive}\n", current.read_text())
        self.assertIn(f"DEB_SHA256={hashlib.sha256(original).hexdigest()}\n", current.read_text())
        saved = current.read_bytes(), current.stat().st_ino
        result = self.adopt(self.pkg_v1)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("already records installed version 1.0.0", result.stdout)
        self.assertEqual((current.read_bytes(), current.stat().st_ino), saved)
        self.assertFalse(previous.exists())
        self.assertEqual(backup.read_bytes(), legacy_helper)
        self.assertTrue(unit.exists())
        self.assertNotIn("apt-get", self.event_log.read_text()[len(before):])
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse(previous.exists())

    def test_adopt_repairs_stale_record_after_external_install_and_preserves_rollback(self) -> None:
        helper, _ = self.legacy_helper()
        legacy_helper = helper.read_bytes()
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        state = self.target / "var/lib/android-auto-receiver/deploy"
        current = state / "current.env"
        previous = state / "previous.env"
        old_current, old_previous = current.read_bytes(), previous.read_bytes()

        # Reproduce dpkg -i outside the deployment tool against the fake target.
        subprocess.run(["dpkg-deb", "--extract", self.pkg_v2, self.target], check=True, capture_output=True)
        status = self.target / "var/lib/dpkg/fake-packages.txt"
        status.write_text(status.read_text().replace("android-auto-receiver\t1.0.0", "android-auto-receiver\t1.1.0"))
        for result in (self.install(self.pkg_v2), self.rollback()):
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertIn("--adopt-installed", result.stderr)
        archive = state / "packages" / self.pkg_v2.name

        # Even a trusted digest for the wrong archive or a same-version rebuild
        # cannot turn it into a rollback artifact for the installed payload.
        archive.write_bytes(self.pkg_v1.read_bytes())
        result = self.adopt(self.pkg_v1)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("cached archive metadata does not match", result.stderr)
        rebuilt = make_deb(self.base / "rebuilt", "1.1.0", b"DIFFERENT PAYLOAD\n")
        archive.write_bytes(rebuilt.read_bytes())
        result = self.adopt(rebuilt)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("installed payload differs", result.stderr)
        self.assertEqual(current.read_bytes(), old_current)
        self.assertEqual(previous.read_bytes(), old_previous)

        archive.write_bytes(self.pkg_v2.read_bytes())
        result = self.adopt(self.pkg_v2)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("VERSION=1.1.0", current.read_text())
        self.assertEqual(previous.read_bytes(), old_previous)
        result = self.rollback()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("rolled_back_to=legacy", result.stdout)
        self.assertEqual(helper.read_bytes(), legacy_helper)

    def test_identical_redeploy_restores_pruned_archive_but_upgrade_refuses(self) -> None:
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        state = self.target / "var/lib/android-auto-receiver/deploy"
        previous = (state / "previous.env").read_bytes()
        archive = state / "packages" / self.pkg_v1.name
        archive.unlink()
        result = self.install(self.pkg_v2)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("current deployment archive missing or corrupt", result.stderr)
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(archive.read_bytes(), self.pkg_v1.read_bytes())
        self.assertEqual((state / "previous.env").read_bytes(), previous)

    def test_identical_redeploy_reinstalls_modified_receiver(self) -> None:
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        receiver = self.target / "usr/bin/aa-wireless-receiver"
        digest = self.target / "etc/android-auto-receiver/receiver.sha256"
        good = receiver.read_bytes()
        for tamper in ("modify", "delete"):
            with self.subTest(tamper=tamper):
                if tamper == "modify":
                    receiver.write_bytes(b"MODIFIED-RECEIVER\n")
                else:
                    receiver.unlink()
                result = self.install()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(receiver.read_bytes(), good)
                self.assertEqual(digest.read_text().strip(), hashlib.sha256(good).hexdigest())

    def test_lock_timeout_reports_holder_check_and_cleans_staging(self) -> None:
        result = self.install(FAKE_LOCK_TIMEOUT="1")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("timed out after 120s", result.stderr)
        self.assertIn(f"sudo fuser -v {self.target}/run/aa-deploy.lock", result.stderr)
        self.assertEqual(list((self.target / "tmp").glob("aa-deploy-stage.*")), [])
        self.assertFalse((self.target / "usr/bin/aa-wireless-receiver").exists())

    def test_failed_removal_preserves_both_clean_and_legacy_recovery(self) -> None:
        state = self.target / "var/lib/android-auto-receiver/deploy"
        digest = self.target / "etc/android-auto-receiver/receiver.sha256"
        for mode in ("none", "legacy"):
            with self.subTest(mode=mode):
                if mode == "legacy":
                    self.legacy_helper()
                result = self.install()
                self.assertEqual(result.returncode, 0, result.stderr)
                files = (state / "current.env", state / "previous.env", digest)
                before = [path.read_bytes() for path in files]
                for failure in ("FAKE_REMOVE_FAIL", "FAKE_REMOVE_NOOP"):
                    result = self.rollback(**{failure: "1"})
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn("removal failed", result.stderr)
                    self.assertEqual([path.read_bytes() for path in files], before)
                result = self.rollback()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"rolled_back_to={mode}", result.stdout)

    def test_legacy_rollback_restores_packaged_overlapping_helper(self) -> None:
        helper, unit = self.legacy_helper()
        old_helper = helper.read_bytes()
        old_unit = unit.read_bytes()
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotEqual(helper.read_bytes(), old_helper)
        result = self.rollback(FAKE_HELPER_ENABLED="enabled")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(os.access(helper, os.X_OK))
        self.assertEqual(helper.read_bytes(), old_helper)
        self.assertEqual(unit.read_bytes(), old_unit)

    def test_rollback_refuses_legacy_unit_without_its_saved_executable(self) -> None:
        helper, unit = self.legacy_helper()
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        state = self.target / "var/lib/android-auto-receiver/deploy"
        recovery = state / "previous.env"
        old_format_backup = state / "rollback/legacy"
        old_format_backup.mkdir(exist_ok=True)
        old_format_backup.joinpath(unit.name).write_text("[Service]\nExecStart=/usr/libexec/aa-system-helper\n")
        recovery.write_text("PREV_MODE=legacy\n")
        result = self.rollback()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("saved legacy helper executable missing", result.stderr)
        self.assertTrue(helper.exists())
        self.assertTrue(recovery.exists())
        self.assertFalse(unit.exists())
        self.assertFalse(any(line.startswith("apt-get remove") for line in self.event_log.read_text().splitlines()))

    def test_restart_failure_keeps_rollback_record_for_retry(self) -> None:
        helper, unit = self.legacy_helper()
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        recovery = self.target / "var/lib/android-auto-receiver/deploy/previous.env"
        expected = recovery.read_bytes()
        result = self.rollback(FAKE_HELPER_ACTIVE="active", FAKE_RESTART_FAIL="1")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("restart failed", result.stderr)
        self.assertEqual(recovery.read_bytes(), expected)
        self.assertTrue(helper.exists())
        self.assertTrue(unit.exists())
        result = self.rollback()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("helper_restarted=1", result.stdout)
        self.assertFalse(recovery.exists())

    def test_rollback_warns_if_infotainment_snapshot_cannot_read_file(self) -> None:
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        unreadable = self.target / "home/zucchi/Desktop/infotainment/unreadable-fixture"
        unreadable.touch(mode=0)
        result = self.install(self.pkg_v2)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        result = self.rollback()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("warning: infotainment snapshot failed", result.stderr)
        self.assertIn("rolled_back_to=none", result.stdout)

    def test_legacy_backup_failure_aborts_before_package_install(self) -> None:
        helper, unit = self.legacy_helper()
        old_helper = helper.read_bytes()
        rule = self.target / "etc/udev/rules.d/70-android-auto-receiver.rules"
        rule.write_text("# legacy rule\n")
        result = self.install(FAKE_LEGACY_BACKUP_FAIL="1")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("legacy backup failed", result.stderr)
        self.assertEqual(rule.read_text(), "# legacy rule\n")
        self.assertTrue(unit.exists())
        self.assertEqual(helper.read_bytes(), old_helper)
        self.assertFalse((self.target / "usr/bin/aa-wireless-receiver").exists())

    def test_metadata_backup_failure_aborts_before_upgrade(self) -> None:
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        state = self.target / "var/lib/android-auto-receiver/deploy"
        records = state / "current.env", state / "previous.env"
        old_records = [record.read_bytes() for record in records]
        receiver = self.target / "usr/bin/aa-wireless-receiver"
        before = self.event_log.read_text()
        result = self.install(self.pkg_v2, FAKE_METADATA_BACKUP_FAIL="1")
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("metadata backup failed", result.stderr)
        self.assertEqual([record.read_bytes() for record in records], old_records)
        self.assertEqual(receiver.read_bytes(), b"ELF-RECEIVER-V1\n")
        self.assertNotIn("apt-get install", self.event_log.read_text()[len(before):])

    def test_preflight_refusals_and_no_secret_leak(self) -> None:
        ap_conf = self.target / "etc/android-auto-receiver/ap.conf"
        session_uid = self.target / "etc/android-auto-receiver/session.uid"

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

        # Wrong dpkg architecture
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_ARCH": "amd64"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("unsupported dpkg architecture", res.stderr)

        # Wrong kernel architecture (uname -m)
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_UNAME_M": "x86_64"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("unsupported kernel arch", res.stderr)

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

        # ap.conf mode 0644 refused (matches Files.cpp:47)
        ap_conf.chmod(0o644)
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("mode too permissive", res.stderr)
        self.assertNotIn("wlan0", res.stdout + res.stderr)
        ap_conf.chmod(0o600)

        # session.uid content must be a decimal UID (IdentityPolicy::load)
        for bad in ("zucchi\n", "\n", "4294967296\n", "-1\n"):
            session_uid.write_text(bad)
            res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
            self.assertNotEqual(res.returncode, 0, msg=bad)
            self.assertIn("session.uid", res.stderr)
        session_uid.write_text("2002\n")

        # Non-root-owned ap.conf refused (matches Files.cpp:46)
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_AP_UID": "1000"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("root-owned", res.stderr)

        # Missing ap.conf and symlinked ap.conf
        ap_conf.unlink()
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("missing or non-regular operator ap.conf", res.stderr)
        ap_conf.symlink_to(session_uid)
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("missing or non-regular operator ap.conf", res.stderr)
        ap_conf.unlink()
        ap_conf.write_text("v1 wlan0 US jurisdiction-confirmed 5277 44\n")
        ap_conf.chmod(0o600)

        # Missing and symlinked session.uid
        session_uid.unlink()
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("missing or non-regular operator session.uid", res.stderr)
        session_uid.symlink_to(ap_conf)
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("missing or non-regular operator session.uid", res.stderr)
        session_uid.unlink()
        session_uid.write_text("2002\n")
        session_uid.chmod(0o600)

        # Unconfirmed jurisdiction in ap.conf (and verify ap.conf contents never leak)
        ap_conf.write_text("v1 wlan0 US jurisdiction-unconfirmed 5277 44\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("jurisdiction-confirmed", res.stderr)
        self.assertNotIn("wlan0", res.stdout + res.stderr)
        self.assertNotIn("5277", res.stdout + res.stderr)

        # Jurisdiction mismatch (use distinctive code "ZZ" that cannot appear in generic error text)
        ap_conf.write_text("v1 wlan0 ZZ jurisdiction-confirmed 5277 44\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("does not match", res.stderr)
        self.assertNotIn("ZZ", res.stdout + res.stderr)

        # Country code "00" rejected (matches Files.cpp:106-107)
        ap_conf.write_text("v1 wlan0 00 jurisdiction-confirmed 5277 44\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("country code", res.stderr)
        self.assertNotIn("wlan0", res.stdout + res.stderr)

        # Privileged port 80 rejected (matches Files.cpp:112 requiring 1024..65535)
        ap_conf.write_text("v1 wlan0 US jurisdiction-confirmed 80 44\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("port out of range", res.stderr)
        self.assertNotIn("wlan0", res.stdout + res.stderr)

        # Invalid interface name (too long / bad char, matches Files.cpp:107-108)
        ap_conf.write_text("v1 wlan0$bad US jurisdiction-confirmed 5277 44\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("interface name invalid", res.stderr)
        self.assertNotIn("wlan0$bad", res.stdout + res.stderr)

        # Invalid channel and multi-line ap.conf
        ap_conf.write_text("v1 wlan0 US jurisdiction-confirmed 5277 52\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("channel invalid", res.stderr)

        ap_conf.write_text("v1 wlan0 US jurisdiction-confirmed 5277 44\nextra\n")
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("schema invalid", res.stderr)

        ap_conf.write_text("v1 wlan0 US jurisdiction-confirmed 5277 44\n")
        ap_conf.chmod(0o600)

        # Staged .deb SHA-256 mismatch refused
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_CORRUPT_SCP": "1"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("staged .deb sha256 mismatch", res.stderr)
        self.assertFalse((self.target / "usr/bin/aa-wireless-receiver").exists())

        # Unsatisfiable Depends: dpkg-query -W recorded before apt-get -s, and no install occurs
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_DEPENDS_FAIL": "1"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("Depends not satisfiable", res.stderr)
        self.assertFalse((self.target / "usr/bin/aa-wireless-receiver").exists())

    def test_failed_install_preserves_legacy_files(self) -> None:
        legacy_udev = self.target / "etc/udev/rules.d/70-android-auto-receiver.rules"
        _, legacy_unit = self.legacy_helper()
        legacy_dbus = self.target / "etc/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf"
        legacy_udev.write_text("# legacy udev\n")
        legacy_dbus.write_text("<busconfig><!-- legacy --></busconfig>\n")

        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_INSTALL_FAIL": "1"},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertTrue(legacy_udev.exists())
        self.assertTrue(legacy_unit.exists())
        self.assertTrue(legacy_dbus.exists())
        self.assertEqual(legacy_udev.read_text(), "# legacy udev\n")
        self.assertEqual(legacy_unit.read_text(), "[Service]\nExecStart=/usr/libexec/aa-system-helper\n")
        self.assertEqual(legacy_dbus.read_text(), "<busconfig><!-- legacy --></busconfig>\n")
        packages = self.target / "var/lib/android-auto-receiver/deploy/packages"
        if packages.exists():
            self.assertEqual(sorted(p.name for p in packages.glob("incoming-*")), [])
            # A failed install never writes the version-keyed rollback artifact.
            self.assertFalse((packages / "android-auto-receiver_1.0.0_arm64.deb").exists())

    def test_install_migration_digest_from_installed_and_no_enable_inactive_helper(self) -> None:
        # Pre-populate legacy ad-hoc install artifacts
        legacy_bin_dir = self.target / "home/zucchi/aa-build/bin"
        legacy_bin_dir.mkdir(parents=True)
        legacy_recv = legacy_bin_dir / "aa-wireless-receiver"
        legacy_recv.write_bytes(b"LEGACY-RECEIVER-DIFFERENT-BYTES\n")
        legacy_recv.chmod(0o755)
        legacy_udev = self.target / "etc/udev/rules.d/70-android-auto-receiver.rules"
        legacy_udev.write_text("# legacy udev\n")
        _, legacy_unit = self.legacy_helper()
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

        # Rollback back to legacy restores byte-identical legacy files, digest, and preserves allow fragment
        rb = self.run_deploy(
            "zucchi@jetson", "--rollback",
            extra_env={"FAKE_HELPER_ACTIVE": "inactive", "FAKE_HELPER_ENABLED": "disabled"},
        )
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertIn("rolled_back_to=legacy", rb.stdout)
        self.assertEqual(legacy_udev.read_text(), "# legacy udev\n")
        self.assertEqual(legacy_unit.read_text(), "[Service]\nExecStart=/usr/libexec/aa-system-helper\n")
        self.assertEqual(legacy_dbus.read_text(), "<busconfig><!-- legacy --></busconfig>\n")
        self.assertEqual(allow_frag.read_text(), "<busconfig><!-- keep --></busconfig>\n")
        self.assertEqual(legacy_sha.read_text(), "0" * 64 + "\n")

    def test_enabled_but_inactive_helper_is_restarted_never_enabled(self) -> None:
        res = self.run_deploy(
            "zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US",
            extra_env={"FAKE_HELPER_ACTIVE": "inactive", "FAKE_HELPER_ENABLED": "enabled"},
        )
        self.assertEqual(res.returncode, 0, res.stderr)
        events = self.event_log.read_text().splitlines()
        self.assertTrue(any(e == "systemctl restart aa-system-helper.service" for e in events), events)
        self.assertFalse(any(e.startswith("systemctl enable") for e in events), events)
        self.assertFalse(any(e.startswith("systemctl start") for e in events), events)

    def test_clean_box_rollback_and_edge_cases(self) -> None:
        # Clean-box install (no legacy files)
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertEqual(res.returncode, 0, res.stderr)
        sha_file = self.target / "etc/android-auto-receiver/receiver.sha256"
        prev_env = self.target / "var/lib/android-auto-receiver/deploy/previous.env"
        self.assertTrue(sha_file.exists())
        self.assertIn("PREV_MODE=none", prev_env.read_text())

        # Rollback on clean box removes package and receiver.sha256, reports rolled_back_to=none, no receiver_sha256
        rb = self.run_deploy("zucchi@jetson", "--rollback")
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertIn("rolled_back_to=none", rb.stdout)
        self.assertNotIn("rolled_back_to=legacy", rb.stdout)
        self.assertNotIn("receiver_sha256=", rb.stdout)
        self.assertFalse(sha_file.exists())
        self.assertFalse((self.target / "usr/bin/aa-wireless-receiver").exists())

        # Second rollback refuses cleanly with no previous deployment recorded
        rb2 = self.run_deploy("zucchi@jetson", "--rollback")
        self.assertNotEqual(rb2.returncode, 0)
        self.assertIn("no previous deployment recorded", rb2.stderr)

    def test_rollback_not_blocked_by_missing_infotainment_or_install_preflight(self) -> None:
        res1 = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertEqual(res1.returncode, 0, res1.stderr)
        res2 = self.run_deploy("zucchi@jetson", str(self.pkg_v2), "--confirm-jurisdiction", "US")
        self.assertEqual(res2.returncode, 0, res2.stderr)

        # Break install-only preflight conditions (ap.conf, disk, OS) AND remove infotainment dir
        (self.target / "etc/android-auto-receiver/ap.conf").unlink()
        (self.target / "etc/os-release").write_text('ID=ubuntu\nVERSION_ID="22.04"\n')
        shutil.rmtree(self.target / "home/zucchi/Desktop/infotainment")

        rb = self.run_deploy(
            "zucchi@jetson", "--rollback",
            extra_env={"FAKE_FREE_MB": "50"},
        )
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertIn("warning: infotainment tree missing", rb.stderr)
        self.assertIn("rolled_back_to=1.0.0", rb.stdout)
        self.assertEqual((self.target / "usr/bin/aa-wireless-receiver").read_bytes(), b"ELF-RECEIVER-V1\n")

    def test_rollback_detects_infotainment_tamper_when_present_and_corrupt_prev_deb(self) -> None:
        res1 = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US")
        self.assertEqual(res1.returncode, 0, res1.stderr)
        res2 = self.run_deploy("zucchi@jetson", str(self.pkg_v2), "--confirm-jurisdiction", "US")
        self.assertEqual(res2.returncode, 0, res2.stderr)

        # Corrupt previous .deb -> rollback refuses
        prev_deb = self.target / "var/lib/android-auto-receiver/deploy/packages/android-auto-receiver_1.0.0_arm64.deb"
        saved_bytes = prev_deb.read_bytes()
        prev_deb.write_bytes(saved_bytes + b"corrupt")
        rb_corrupt = self.run_deploy("zucchi@jetson", "--rollback")
        self.assertNotEqual(rb_corrupt.returncode, 0)
        self.assertIn("previous .deb sha256 mismatch", rb_corrupt.stderr)

        # Missing previous .deb -> rollback refuses
        prev_deb.unlink()
        rb_missing = self.run_deploy("zucchi@jetson", "--rollback")
        self.assertNotEqual(rb_missing.returncode, 0)
        self.assertIn("recorded previous .deb missing", rb_missing.stderr)

        # Restore previous .deb and verify infotainment tamper during rollback still fails
        prev_deb.write_bytes(saved_bytes)
        rb_tamper = self.run_deploy(
            "zucchi@jetson", "--rollback",
            extra_env={"FAKE_TAMPER_INFOTAINMENT": "1"},
        )
        self.assertNotEqual(rb_tamper.returncode, 0)
        self.assertIn("infotainment tree modified", rb_tamper.stderr)

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
        # Dead receiver.sha256.prev is no longer written
        self.assertFalse((self.target / "var/lib/android-auto-receiver/deploy/rollback/receiver.sha256.prev").exists())

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
        self.assertIn("package and configuration changes completed", res.stderr)
        self.assertTrue((self.target / "var/lib/android-auto-receiver/deploy/previous.env").exists())

    def test_infotainment_snapshots_disable_git_index_refresh(self) -> None:
        result = self.install()
        self.assertEqual(result.returncode, 0, result.stderr)
        statuses = [line for line in self.event_log.read_text().splitlines() if " status --porcelain" in line]
        self.assertEqual(len(statuses), 2)
        self.assertTrue(all(line.startswith("git --no-optional-locks ") for line in statuses), statuses)

    def test_dry_run_prints_real_restart_decision_without_running_ssh(self) -> None:
        res = self.run_deploy("zucchi@jetson", str(self.pkg_v1), "--confirm-jurisdiction", "US", "--dry-run")
        self.assertEqual(res.returncode, 0, res.stderr)
        self.assertIn("[dry-run] ssh", res.stdout)
        self.assertIn("[dry-run] scp", res.stdout)
        self.assertIn("apt-get install", res.stdout)
        self.assertIn('if [ "$should_restart" = "1" ]', res.stdout)
        self.assertNotIn('if [ 0 = "1" ]', res.stdout)
        self.assertFalse(self.ssh_log.exists())
        self.assertFalse(self.event_log.exists())

        rb = self.run_deploy("zucchi@jetson", "--rollback", "--dry-run")
        self.assertEqual(rb.returncode, 0, rb.stderr)
        self.assertIn("[dry-run] ssh", rb.stdout)
        self.assertIn("--allow-downgrades", rb.stdout)
        self.assertIn('if [ "$effective_restart" = "1" ]', rb.stdout)
        self.assertNotIn('if [ 0 = "1" ]', rb.stdout)
        self.assertFalse(self.ssh_log.exists())


if __name__ == "__main__":
    unittest.main()
