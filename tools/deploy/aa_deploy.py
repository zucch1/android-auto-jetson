#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Idempotent SSH install, upgrade, and rollback for android-auto-receiver."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import textwrap

PACKAGE_NAME = "android-auto-receiver"
EXPECTED_ARCH = "arm64"
EXPECTED_UNAME_M = "aarch64"
EXPECTED_OS_ID = "ubuntu"
EXPECTED_OS_VERSION = "24.04"
DEFAULT_MIN_FREE_MB = 512
DEFAULT_STATE_DIR = "/var/lib/android-auto-receiver/deploy"
DEFAULT_INFOTAINMENT_DIR = "$HOME/Desktop/infotainment"
INSTALLED_RECEIVER = "/usr/bin/aa-wireless-receiver"
ETC_DIR = "/etc/android-auto-receiver"
RECEIVER_SHA256 = f"{ETC_DIR}/receiver.sha256"
HELPER_UNIT = "aa-system-helper.service"
LEGACY_UDEV_RULE = "/etc/udev/rules.d/70-android-auto-receiver.rules"
LEGACY_HELPER_UNIT = "/etc/systemd/system/aa-system-helper.service"
LEGACY_DBUS_POLICY = "/etc/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf"
JURISDICTION_RE = re.compile(r"^[A-Z]{2}$")
VERSION_RE = re.compile(r"^[0-9A-Za-z.+~:-]+$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class DeployError(RuntimeError):
    """Actionable deployment failure."""


class Runner:
    """Executes or prints local, ssh, and scp commands."""

    def __init__(self, target: str, ssh_opts: list[str], dry_run: bool) -> None:
        self.target = target
        self.ssh_opts = ["-o", "BatchMode=yes", *ssh_opts]
        self.dry_run = dry_run

    def ssh_cmd(self) -> list[str]:
        return ["ssh", *self.ssh_opts, "--", self.target, "bash", "-s"]

    def scp_cmd(self, local_path: Path, remote_path: str) -> list[str]:
        return ["scp", *self.ssh_opts, "--", str(local_path), f"{self.target}:{remote_path}"]

    def ssh(self, label: str, remote_script: str, *, capture: bool = True) -> str:
        cmd = self.ssh_cmd()
        if self.dry_run:
            print(f"[dry-run] # {label}")
            print(f"[dry-run] {shlex.join(cmd)} <<'AA_DEPLOY_EOF'")
            print(remote_script.rstrip())
            print("AA_DEPLOY_EOF")
            return ""
        try:
            proc = subprocess.run(
                cmd,
                input=remote_script,
                text=True,
                errors="replace",
                capture_output=capture,
                check=False,
            )
        except OSError as exc:
            raise DeployError(f"{label} failed: could not run ssh: {exc}") from exc
        if proc.returncode != 0:
            stderr = (proc.stderr or "").strip()
            stdout = (proc.stdout or "").strip()
            detail = stderr or stdout or f"exit status {proc.returncode}"
            raise DeployError(f"{label} failed: {detail}")
        return proc.stdout if capture else ""

    def scp(self, label: str, local_path: Path, remote_path: str) -> None:
        cmd = self.scp_cmd(local_path, remote_path)
        if self.dry_run:
            print(f"[dry-run] # {label}")
            print(f"[dry-run] {shlex.join(cmd)}")
            return
        try:
            proc = subprocess.run(
                cmd,
                text=True,
                errors="replace",
                capture_output=True,
                check=False,
            )
        except OSError as exc:
            raise DeployError(f"{label} failed: could not run scp: {exc}") from exc
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout or f"exit status {proc.returncode}").strip()
            raise DeployError(f"{label} failed: {detail}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def inspect_local_deb(deb_path: Path) -> dict[str, str]:
    if not deb_path.is_file():
        raise DeployError(f"package file not found: {deb_path}")
    fields: dict[str, str] = {}
    for field in ("Package", "Version", "Architecture", "Depends"):
        try:
            proc = subprocess.run(
                ["dpkg-deb", "--field", "--", str(deb_path), field],
                text=True,
                errors="replace",
                capture_output=True,
                check=False,
            )
        except OSError as exc:
            raise DeployError(f"could not run dpkg-deb on {deb_path}: {exc}") from exc
        if proc.returncode != 0:
            detail = (proc.stderr or proc.stdout or f"exit status {proc.returncode}").strip()
            raise DeployError(f"dpkg-deb failed on {deb_path}: {detail}")
        fields[field] = proc.stdout.strip()
    if fields["Package"] != PACKAGE_NAME:
        raise DeployError(f"unexpected package name {fields['Package']!r} (expected {PACKAGE_NAME!r})")
    if fields["Architecture"] != EXPECTED_ARCH:
        raise DeployError(
            f"unexpected package architecture {fields['Architecture']!r} (expected {EXPECTED_ARCH!r})"
        )
    if not fields["Version"] or not VERSION_RE.fullmatch(fields["Version"]):
        raise DeployError(f"invalid Version field {fields['Version']!r} in {deb_path}")
    fields["SHA256"] = sha256_file(deb_path)
    return fields


def remote_path_expr(path: str) -> str:
    """Quote a remote path while allowing a leading $HOME/ expansion."""
    if path == "$HOME":
        return '"$HOME"'
    if path.startswith("$HOME/"):
        return '"$HOME/"' + shlex.quote(path[len("$HOME/"):])
    return shlex.quote(path)


# Runs on the target as root. Mirror the production readers in Identity.cpp
# (protected_file, IdentityPolicy::load) and nm/Files.cpp (directory, read_into,
# load_policy). receiver.sha256 is generated separately from the installed binary.
OPERATOR_CONFIG_VALIDATOR = r"""
import os
import re
import stat
import sys
from contextlib import ExitStack

root, jurisdiction = sys.argv[1:]

def refuse(reason):
    sys.exit("preflight: " + reason)

def open_checked(stack, name, flags, reason, *, dir_fd=None):
    try:
        fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | flags, dir_fd=dir_fd)
        stack.callback(os.close, fd)
        return fd, os.fstat(fd)
    except OSError:
        refuse(reason)

def read_config(name, limit, mode_mask, *, single_link=False):
    with ExitStack() as stack:
        fd, st = open_checked(
            stack, name, os.O_NONBLOCK, f"missing or non-regular operator {name}", dir_fd=directory,
        )
        if not stat.S_ISREG(st.st_mode):
            refuse(f"missing or non-regular operator {name}")
        if st.st_uid != 0 or st.st_mode & mode_mask:
            refuse(f"operator {name} ownership or mode too permissive (must be root-owned and protected)")
        if single_link and st.st_nlink != 1:
            refuse(f"operator {name} must not be hard-linked")
        if single_link and st.st_size > limit:
            refuse(f"operator {name} exceeds {limit} bytes")
        try:
            if single_link:
                data = os.read(fd, limit + 1)
                if len(data) != st.st_size:
                    refuse(f"operator {name} size changed while reading")
            else:
                with os.fdopen(fd, "rb", closefd=False) as stream:
                    data = stream.read(limit + 1)
        except OSError:
            refuse(f"could not read operator {name}")
        if len(data) > limit:
            refuse(f"operator {name} exceeds {limit} bytes")
        return data

with ExitStack() as stack:
    directory, _ = open_checked(stack, root, os.O_DIRECTORY, "could not open target root")
    for component in ("etc", "android-auto-receiver"):
        directory, st = open_checked(
            stack, component, os.O_DIRECTORY, "missing or non-directory operator config dir",
            dir_fd=directory,
        )
        if st.st_uid != 0 or st.st_mode & 0o022:
            refuse("operator config dir must be root-owned and not group/other-writable")
    ap = read_config("ap.conf", 512, 0o077, single_link=True)
    uid = read_config("session.uid", 128, 0o022).removesuffix(b"\n")

if not re.fullmatch(rb"[0-9]+", uid):
    refuse("operator session.uid must hold a decimal UID")
if int(uid) > 0xffffffff:
    refuse("operator session.uid is out of uint32 range")

fields = ap.split()
if len(fields) not in (5, 6):
    refuse("ap.conf schema invalid (expected 5 or 6 fields)")
version, interface, country, confirmed, port, *channel = fields
if version != b"v1":
    refuse("ap.conf schema version invalid")
if not re.fullmatch(rb"[A-Za-z0-9_-]{1,15}", interface):
    refuse("ap.conf interface name invalid")
if not re.fullmatch(rb"[A-Z]{2}", country):
    refuse("ap.conf country code malformed")
if country != jurisdiction.encode("ascii"):
    refuse("--confirm-jurisdiction does not match operator ap.conf")
if confirmed != b"jurisdiction-confirmed":
    refuse("ap.conf does not have jurisdiction-confirmed")
if not re.fullmatch(rb"[0-9]+", port):
    refuse("ap.conf port invalid")
if not 1024 <= int(port) <= 65535:
    refuse("ap.conf port out of range (expected 1024..65535)")
if channel and (
    not re.fullmatch(rb"[0-9]+", channel[0]) or int(channel[0]) not in (36, 40, 44, 48)
):
    refuse("ap.conf channel invalid (expected 36, 40, 44 or 48)")
"""


def build_preflight_script(
    jurisdiction: str | None,
    min_free_mb: int,
    state_dir: str,
) -> str:
    q_jur = shlex.quote(jurisdiction or "")
    q_state_parent = shlex.quote(str(Path(state_dir).parent))
    return textwrap.dedent(f"""\
        set -euo pipefail
        if [ -e "$HOME/MEASUREMENT_IN_PROGRESS" ]; then
            echo "preflight: measurement in progress ($HOME/MEASUREMENT_IN_PROGRESS exists)" >&2
            exit 1
        fi
        arch="$(dpkg --print-architecture)"
        if [ "$arch" != {shlex.quote(EXPECTED_ARCH)} ]; then
            echo "preflight: unsupported dpkg architecture: $arch (expected {EXPECTED_ARCH})" >&2
            exit 1
        fi
        machine="$(uname -m)"
        if [ "$machine" != {shlex.quote(EXPECTED_UNAME_M)} ]; then
            echo "preflight: unsupported kernel arch: $machine (expected {EXPECTED_UNAME_M})" >&2
            exit 1
        fi
        if [ ! -r /etc/os-release ]; then
            echo "preflight: /etc/os-release missing" >&2
            exit 1
        fi
        os_id="$(awk -F= '$1=="ID"{{gsub(/"/, "", $2); print $2; exit}}' /etc/os-release)"
        os_ver="$(awk -F= '$1=="VERSION_ID"{{gsub(/"/, "", $2); print $2; exit}}' /etc/os-release)"
        if [ "$os_id" != {shlex.quote(EXPECTED_OS_ID)} ] || [ "$os_ver" != {shlex.quote(EXPECTED_OS_VERSION)} ]; then
            echo "preflight: unsupported OS $os_id $os_ver (expected {EXPECTED_OS_ID} {EXPECTED_OS_VERSION})" >&2
            exit 1
        fi
        for mp in / /var {q_state_parent}; do
            [ -d "$mp" ] || continue
            avail_mb="$(df -Pm "$mp" | awk 'NR==2{{print $4}}')"
            if [ -z "$avail_mb" ] || [ "$avail_mb" -lt {int(min_free_mb)} ]; then
                echo "preflight: insufficient free disk on $mp: ${{avail_mb:-0}} MiB (need {int(min_free_mb)} MiB)" >&2
                exit 1
            fi
        done
        sudo -n python3 -I - / {q_jur} <<'AA_OPERATOR_CONFIG'
    """) + OPERATOR_CONFIG_VALIDATOR + textwrap.dedent("""\
        AA_OPERATOR_CONFIG
        echo "preflight=ok"
    """)


def build_recovery_preflight_script(state_dir: str) -> str:
    """Check architecture, the measurement marker and the recovery state dir."""
    script = textwrap.dedent(f"""\
        set -euo pipefail
        if [ -e "$HOME/MEASUREMENT_IN_PROGRESS" ]; then
            echo "preflight: measurement in progress ($HOME/MEASUREMENT_IN_PROGRESS exists)" >&2
            exit 1
        fi
        arch="$(dpkg --print-architecture)"
        if [ "$arch" != {shlex.quote(EXPECTED_ARCH)} ]; then
            echo "preflight: unsupported dpkg architecture: $arch (expected {EXPECTED_ARCH})" >&2
            exit 1
        fi
        machine="$(uname -m)"
        if [ "$machine" != {shlex.quote(EXPECTED_UNAME_M)} ]; then
            echo "preflight: unsupported kernel arch: $machine (expected {EXPECTED_UNAME_M})" >&2
            exit 1
        fi
    """)
    q_state = shlex.quote(state_dir)
    script += textwrap.dedent(f"""\
        if ! sudo -n test -d {q_state}; then
            echo "preflight: deploy state directory missing:" {q_state} >&2
            exit 1
        fi
    """)
    return script


def build_infotainment_snapshot_script(infotainment_dir: str, *, optional: bool = False) -> str:
    dir_expr = remote_path_expr(infotainment_dir)
    if optional:
        missing_dir_block = textwrap.dedent("""\
            if [ ! -d "$dir" ] || ! git -C "$dir" rev-parse HEAD >/dev/null 2>&1; then
                printf 'INFOTAINMENT_STATUS=missing\\n'
                exit 0
            fi
        """)
    else:
        missing_dir_block = textwrap.dedent("""\
            if [ ! -d "$dir" ]; then
                echo "infotainment: directory not found: $dir" >&2
                exit 1
            fi
        """)
    return textwrap.dedent(f"""\
        set -euo pipefail
        dir={dir_expr}
    """) + missing_dir_block + textwrap.dedent("""\
        head_sha="$(git -C "$dir" rev-parse HEAD)"
        status_sha="$(git --no-optional-locks -C "$dir" status --porcelain | sha256sum | cut -d' ' -f1)"
        tree_sha="$(
            cd "$dir" &&
            find . -path './.git' -prune -o -type f -print0 |
            LC_ALL=C sort -z |
            xargs -0 -r sha256sum |
            sha256sum | cut -d' ' -f1
        )"
        printf 'INFOTAINMENT_STATUS=present\\nHEAD=%s\\nSTATUS_SHA256=%s\\nTREE_SHA256=%s\\n' "$head_sha" "$status_sha" "$tree_sha"
    """)


def build_record_before_script(state_dir: str, run_dir: str, info_before: str) -> str:
    q_state = shlex.quote(state_dir)
    q_run = shlex.quote(run_dir)
    q_info = shlex.quote(info_before)
    return textwrap.dedent(f"""\
        set -euo pipefail
        sudo -n install -d -m 0700 -o root -g root {q_state} {q_state}/runs {q_state}/packages {q_state}/rollback {q_run}
        dpkg-query -W | sudo -n tee {q_run}/dpkg-query-before.txt >/dev/null
        sudo -n chmod 0600 {q_run}/dpkg-query-before.txt
        printf '%s' {q_info} | sudo -n tee {q_run}/infotainment-before.txt >/dev/null
        sudo -n chmod 0600 {q_run}/infotainment-before.txt
    """)


def parse_kv(output: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in output.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip()
    return result


def build_depends_check_script(stage_dir: str, staged_deb: str, expected_sha: str) -> str:
    q_stage = shlex.quote(stage_dir)
    q_deb = shlex.quote(staged_deb)
    q_sha = shlex.quote(expected_sha)
    return textwrap.dedent(f"""\
        set -euo pipefail
        actual_sha="$(sha256sum {q_deb} | cut -d' ' -f1)"
        if [ "$actual_sha" != {q_sha} ]; then
            rm -rf {q_stage}
            echo "transfer: staged .deb sha256 mismatch" >&2
            exit 1
        fi
        if ! sim_out="$(apt-get -s -y install {q_deb} 2>&1)"; then
            rm -rf {q_stage}
            echo "preflight: package Depends not satisfiable on target: $sim_out" >&2
            exit 1
        fi
        echo "depends=ok"
    """)


# Serialize mutations even when callers choose different state directories.
def locked_mutation(script: str, *, stage_dir: str | None = None) -> str:
    cleanup = ""
    if stage_dir is not None:
        cleanup = f"trap {shlex.quote('rm -rf -- ' + shlex.quote(stage_dir))} EXIT\n"
    return (
        'set -euo pipefail\n'
        + cleanup
        + 'rc=0\n'
        'sudo -n flock --close --timeout 120 --conflict-exit-code 75 /run/aa-deploy.lock '
        'env AA_DEPLOY_OPERATOR_HOME="$HOME" bash -s <<\'AA_LOCKED_MUTATION\' || rc=$?\n'
        + script
        + "AA_LOCKED_MUTATION\n"
        'if [ "$rc" -eq 75 ]; then\n'
        '    echo "timed out after 120s waiting for /run/aa-deploy.lock; '
        'on the target, inspect the holder with sudo fuser -v /run/aa-deploy.lock and retry when it exits" >&2\n'
        'fi\n'
        'exit "$rc"\n'
    )


DEPLOY_STATE_FUNCTIONS = r"""
fail() { echo "$*" >&2; exit 1; }

state_field() {
    awk -v key="$2=" 'index($0, key) == 1 { print substr($0, length(key) + 1); exit }' "$1"
}

read_installed_version() {
    local record status rc
    installed_version=""
    if record="$(dpkg-query -W -f='${db:Status-Status} ${Version}\n' android-auto-receiver 2>/dev/null)"; then
        read -r status installed_version <<< "$record"
        case "$status" in
            installed) [ -n "$installed_version" ] || fail "dpkg reported an empty installed version" ;;
            config-files|not-installed) installed_version="" ;;
            *) fail "package is in dpkg state $status; run sudo dpkg --configure -a on the target, resolve any dpkg errors, then retry" ;;
        esac
    else
        rc=$?
        [[ "$rc" -eq 1 && -z "$record" ]] || fail "could not read installed package status"
    fi
}

read_current_metadata() {
    current_version=""
    current_sha=""
    current_deb=""
    if [ -f "$current_env" ]; then
        current_version="$(state_field "$current_env" VERSION)"
        current_sha="$(state_field "$current_env" DEB_SHA256)"
        current_deb="$(state_field "$current_env" DEB_PATH)"
    fi
    if [ ! -f "$current_env" ] || [ "$(state_field "$current_env" PREV_MODE)" != deb ] ||
        [[ ! "$current_version" =~ ^[0-9A-Za-z.+~:-]+$ || ! "$current_sha" =~ ^[a-f0-9]{64}$ ]] ||
        [ -z "$current_deb" ] || [ "$current_version" != "$installed_version" ]; then
        fail "missing or inconsistent deployment metadata at $current_env; run aa-deploy.sh TARGET --adopt-installed SHA256 (see docs/deploy.md)"
    fi
}

helper_needs_restart() {
    local active enabled
    active="$(systemctl is-active aa-system-helper.service 2>/dev/null || true)"
    enabled="$(systemctl is-enabled aa-system-helper.service 2>/dev/null || true)"
    should_restart=0
    if [ "$active" = active ] || [ "$enabled" = enabled ]; then
        should_restart=1
    fi
}

write_receiver_digest() {
    local temporary
    [[ -f "$receiver" && ! -L "$receiver" ]] || fail "deployed receiver is missing at $receiver"
    digest="$(sha256sum "$receiver" | cut -d' ' -f1)"
    [[ "$digest" =~ ^[a-f0-9]{64}$ ]] || fail "could not compute the deployed receiver digest"
    temporary="$(mktemp -- "$etc_sha.XXXXXX")"
    printf '%s\n' "$digest" > "$temporary"
    chown root:root "$temporary"
    mv -fT -- "$temporary" "$etc_sha"
}
"""

# Accept a single literal absolute ExecStart in [Service], the layout needed
# for the reference unit and ad-hoc units pointing into ~/aa-build/bin.
LEGACY_UNIT_EXECUTABLE = r"""
import re
import sys
from pathlib import Path

root, filename = sys.argv[1:]
try:
    lines = Path(filename).read_text().split("\n")
except (OSError, UnicodeError):
    sys.exit("could not read saved legacy helper unit")

section = ""
commands = []
for line in lines:
    line = line.strip()
    if not line or line.startswith(("#", ";")):
        continue
    if line.endswith("\\"):
        sys.exit("legacy helper unit has unsupported line continuations; recover it manually")
    if line.startswith("["):
        section = line
    key, _, value = line.partition("=")
    if section == "[Service]" and key.rstrip() == "ExecStart":
        commands.append(value.strip())
if len(commands) != 1 or not re.fullmatch(r"/[a-zA-Z0-9_./-]+", commands[0]):
    sys.exit("legacy helper unit needs one literal absolute ExecStart path; recover it manually")
if any(part in ("", ".", "..") for part in commands[0][1:].split("/")):
    sys.exit("legacy helper ExecStart path is not canonical; recover it manually")
print(root.rstrip("/") + commands[0])
"""


# Walk the legacy ExecStart one component at a time and refuse the layout if
# any symlink hop, including a symlinked parent directory, or the final file is
# a path the incoming package installs. APT would replace that hop and break
# the chain rollback relies on to restore the legacy unit.
LEGACY_PACKAGE_OVERLAP_CHECK = r"""
import os
from pathlib import Path
import sys
import tarfile

root, executable = map(Path, sys.argv[1:])


def visited_paths(path):
    visited = []
    current = Path("/")
    pending = list(path.parts[1:])
    hops = 0
    while pending:
        part = pending.pop(0)
        if part in ("", "."):
            continue
        if part == "..":
            current = current.parent
            continue
        candidate = current / part
        if candidate.is_symlink():
            hops += 1
            if hops > 40:
                raise OSError(f"too many symlink levels at {candidate}")
            visited.append(candidate)
            target = Path(os.readlink(candidate))
            if target.is_absolute():
                current = Path("/")
                pending = list(target.parts[1:]) + pending
            else:
                pending = list(target.parts) + pending
        else:
            current = candidate
    if not current.is_file():
        raise OSError(f"resolved legacy helper {current} is not a regular file")
    visited.append(current)
    return {path.relative_to(root) for path in visited if path.is_relative_to(root)}


try:
    paths = visited_paths(executable)
    with tarfile.open(fileobj=sys.stdin.buffer, mode="r|") as payload:
        for member in payload:
            path = Path(member.name)
            if path in paths:
                sys.exit(f"legacy helper executable overlaps a package-owned file: /{path}; migrate it manually")
except (OSError, ValueError, tarfile.TarError) as error:
    sys.exit(f"could not check legacy helper against the incoming package: {error}")
"""


def mutation_script(state_dir: str, run_dir: str) -> str:
    return textwrap.dedent(f"""\
        set -euo pipefail
        export LC_ALL=C
        umask 077
        state_dir={shlex.quote(state_dir)}
        run_dir={shlex.quote(run_dir)}
        receiver={shlex.quote(INSTALLED_RECEIVER)}
        etc_sha={shlex.quote(RECEIVER_SHA256)}
        legacy_udev={shlex.quote(LEGACY_UDEV_RULE)}
        legacy_unit={shlex.quote(LEGACY_HELPER_UNIT)}
        legacy_dbus={shlex.quote(LEGACY_DBUS_POLICY)}
        packaged_helper=/usr/libexec/aa-system-helper
        current_env="$state_dir/current.env"
        prev_env="$state_dir/previous.env"
        restart_pending="$state_dir/rollback/restart-pending"
        legacy_files=("$legacy_udev" "$legacy_unit" "$legacy_dbus" "$etc_sha")
    """) + DEPLOY_STATE_FUNCTIONS + (
        "legacy_executable() {\n"
        "    python3 -I - / \"$1\" <<'AA_LEGACY_UNIT'\n"
        + LEGACY_UNIT_EXECUTABLE
        + "AA_LEGACY_UNIT\n}\n"
    )


# Compare the archive payload to the target without extracting files as root.
ADOPT_PAYLOAD_VERIFIER = r"""
import hashlib
import os
from pathlib import Path, PurePosixPath
import stat
import sys
import tarfile

root = Path(sys.argv[1])


def digest(stream):
    result = hashlib.sha256()
    for chunk in iter(lambda: stream.read(1024 * 1024), b""):
        result.update(chunk)
    return result.digest()


try:
    packaged_paths = set()
    with tarfile.open(fileobj=sys.stdin.buffer, mode="r|") as payload:
        for member in payload:
            name = PurePosixPath(member.name)
            if name.is_absolute() or ".." in name.parts:
                sys.exit(f"adopt: unsafe archive path {member.name!r}")
            path = root / name
            packaged_paths.add(path)
            if member.isdir():
                matches = path.is_dir()
            elif member.issym():
                matches = path.is_symlink() and os.readlink(path) == member.linkname
            elif member.isfile() and stat.S_ISREG(path.lstat().st_mode):
                matches = stat.S_IMODE(path.stat().st_mode) == member.mode
                if matches:
                    with path.open("rb") as installed, payload.extractfile(member) as archived:
                        matches = digest(installed) == digest(archived)
            else:
                matches = False
            if not matches:
                sys.exit(f"adopt: installed payload differs at {member.name!r}")

    # Check machine-wide overrides: unit files or mask symlinks that shadow
    # the packaged units, and drop-ins, including dash-prefix, service-wide
    # and systemctl set-property (system.control) ones. The bare "service"
    # entry rejects every host-wide service.d drop-in, even one unrelated to
    # this package; that fails safe and the operator reviews it. Per-user
    # projection overrides are operator configuration and are not checked.
    unexpected = []
    units = {
        "system": ("aa-system-helper.service", "aa-system-.service", "aa-.service", "service"),
        "user": ("aa-receiver.service", "aa-.service", "service"),
    }
    directories = {
        "system": ("etc/systemd/system.control", "run/systemd/system.control",
                   "etc/systemd/system", "run/systemd/system",
                   "usr/local/lib/systemd/system", "usr/lib/systemd/system"),
        "user": ("etc/systemd/user", "run/systemd/user",
                 "usr/local/lib/systemd/user", "usr/lib/systemd/user"),
    }
    for scope, names in units.items():
        for base in directories[scope]:
            unit_file = root / base / names[0]
            if (unit_file.exists() or unit_file.is_symlink()) and unit_file not in packaged_paths:
                unexpected.append(unit_file.relative_to(root))
            for unit in names:
                directory = root / base / f"{unit}.d"
                unexpected.extend(path.relative_to(root) for path in directory.glob("*.conf")
                                  if path not in packaged_paths)
    if unexpected:
        sys.exit("adopt: unexpected machine-wide systemd drop-ins or unit overrides: " +
                 ", ".join(f"/{path}" for path in sorted(unexpected)) +
                 "; review and resolve these overrides before adoption")
except (OSError, EOFError, ValueError, tarfile.TarError) as error:
    sys.exit(f"adopt: could not verify installed payload: {error}")
"""


def build_adopt_script(state_dir: str, run_dir: str, expected_sha: str) -> str:
    return locked_mutation(mutation_script(state_dir, run_dir) + textwrap.dedent(f"""\
        expected_sha={shlex.quote(expected_sha)}
        for directory in "$state_dir" "$state_dir/packages" "$state_dir/runs"; do
            if [[ "$directory" = "$state_dir/runs" && ! -e "$directory" && ! -L "$directory" ]]; then
                continue
            fi
            [[ -d "$directory" && ! -L "$directory" && "$(stat -c '%u %a' -- "$directory")" = '0 700' ]] ||
                fail "adopt: expected a root-owned mode-0700 directory at $directory"
        done
        read_installed_version
        [ -n "$installed_version" ] ||
            fail "adopt: android-auto-receiver is not installed; deploy normally on a clean target, or follow the incomplete rollback procedure in docs/deploy.md if records remain"
        [[ "$installed_version" =~ ^[0-9A-Za-z.+~:-]+$ ]] ||
            fail "adopt: dpkg reported an invalid version; repair the package state before retrying"

        archive="$state_dir/packages/{PACKAGE_NAME}_${{installed_version}}_{EXPECTED_ARCH}.deb"
        recovery="restore the original {EXPECTED_ARCH} .deb for installed version $installed_version (SHA-256 $expected_sha) to $archive, then retry --adopt-installed $expected_sha; see docs/deploy.md for target commands"
        [[ -f "$archive" && ! -L "$archive" ]] ||
            fail "adopt: missing or non-regular cached archive at $archive; $recovery"
        [ "$(sha256sum -- "$archive" | cut -d' ' -f1)" = "$expected_sha" ] ||
            fail "adopt: cached archive SHA-256 mismatch at $archive; $recovery"
        if [[ "$(dpkg-deb --field -- "$archive" Package)" != {PACKAGE_NAME} ||
              "$(dpkg-deb --field -- "$archive" Version)" != "$installed_version" ||
              "$(dpkg-deb --field -- "$archive" Architecture)" != {EXPECTED_ARCH} ]]; then
            fail "adopt: cached archive metadata does not match the installed package; $recovery"
        fi
        if ! dpkg-deb --fsys-tarfile "$archive" |
            python3 -I -c {shlex.quote(ADOPT_PAYLOAD_VERIFIER)} /; then
            fail "adopt: resolve the reported differences before retrying --adopt-installed $expected_sha; for altered or missing payload files, reinstall the verified archive with sudo dpkg -i -- '$archive' on the target (see docs/deploy.md)"
        fi
        read_installed_version
        [ "$archive" = "$state_dir/packages/{PACKAGE_NAME}_${{installed_version}}_{EXPECTED_ARCH}.deb" ] ||
            fail "adopt: installed version changed during verification; retry after package-manager activity ends"

        install -d -m 0700 -o root -g root -- "$state_dir/runs"
        mkdir -m 0700 -- "$run_dir"
        dpkg-query -W > "$run_dir/dpkg-query-before.txt"
        printf 'run_dir=%s\n' "$run_dir"
        if [[ -f "$current_env" && ! -L "$current_env" &&
            "$(state_field "$current_env" PREV_MODE)" = deb &&
            "$(state_field "$current_env" VERSION)" = "$installed_version" &&
            "$(state_field "$current_env" DEB_PATH)" = "$archive" &&
            "$(state_field "$current_env" DEB_SHA256)" = "$expected_sha" ]]; then
            printf 'current.env already records installed version %s\n' "$installed_version"
            exit 0
        fi
        printf 'PREV_MODE=deb\nVERSION=%s\nDEB_PATH=%s\nDEB_SHA256=%s\n' \
            "$installed_version" "$archive" "$expected_sha" > "$run_dir/current.env"
        mv -fT -- "$run_dir/current.env" "$current_env"
        printf 'recovered current.env for installed version %s\n' "$installed_version"
        installed_digest="$(sha256sum -- {shlex.quote(INSTALLED_RECEIVER)} 2>/dev/null | cut -d' ' -f1 || true)"
        recorded_digest="$(head -c 65 -- {shlex.quote(RECEIVER_SHA256)} 2>/dev/null | tr -d '\n' || true)"
        if [[ ! "$installed_digest" =~ ^[a-f0-9]{{64}}$ || "$installed_digest" != "$recorded_digest" ]]; then
            printf 'adopt: WARNING {RECEIVER_SHA256} is missing or does not match {INSTALLED_RECEIVER}; aa-system-helper will refuse to start until you redeploy the identical package to refresh it (see docs/deploy.md)\n'
        fi
    """))


def build_install_script(
    state_dir: str,
    run_dir: str,
    stage_dir: str,
    staged_deb: str,
    deb_info: dict[str, str],
) -> str:
    version = deb_info["Version"]
    deb_sha = deb_info["SHA256"]
    stored_deb = f"{state_dir}/packages/{PACKAGE_NAME}_{version}_{EXPECTED_ARCH}.deb"
    return locked_mutation(mutation_script(state_dir, run_dir) + textwrap.dedent(f"""\
        version={shlex.quote(version)}
        deb_sha={shlex.quote(deb_sha)}
        stored_deb={shlex.quote(stored_deb)}
        incoming_deb={shlex.quote(f"{state_dir}/packages/incoming-{deb_sha}.deb")}
        stage_dir={shlex.quote(stage_dir)}
        trap 'rm -rf -- "$stage_dir"; rm -f -- "$incoming_deb"' EXIT

        helper_needs_restart
        read_installed_version
        repair_archive=0
        if [ -n "$installed_version" ]; then
            read_current_metadata
            if dpkg --compare-versions "$installed_version" eq "$version" && [ "$current_sha" != "$deb_sha" ]; then
                fail "Debian version $version is already installed with different bytes; build with a new version"
            fi
            if [[ "$installed_version" = "$version" && "$current_deb" = "$stored_deb" &&
                  "$current_sha" = "$deb_sha" && ! -L "$stored_deb" &&
                  ( ! -e "$stored_deb" || -f "$stored_deb" ) ]]; then
                repair_archive=1
            else
                [[ -f "$current_deb" && ! -L "$current_deb" &&
                    "$(sha256sum "$current_deb" | cut -d' ' -f1)" = "$current_sha" ]] ||
                    fail "current deployment archive missing or corrupt at $current_deb; restore the exact archive (SHA-256 $current_sha) using docs/deploy.md, then retry"
            fi
        elif [ -e "$current_env" ] || [ -e "$prev_env" ]; then
            fail "inconsistent deployment metadata at $state_dir with no installed package; follow the incomplete rollback procedure in docs/deploy.md before deploying"
        fi

        # Version-keyed archives are immutable, including those used by an older rollback record.
        if [[ "$repair_archive" = 0 && ( -e "$stored_deb" || -L "$stored_deb" ) ]]; then
            [[ -f "$stored_deb" && ! -L "$stored_deb" &&
                "$(sha256sum "$stored_deb" | cut -d' ' -f1)" = "$deb_sha" ]] ||
                fail "Debian version $version is already cached with different bytes; build with a new version"
        fi

        install -m 0644 -o root -g root {shlex.quote(staged_deb)} "$incoming_deb"
        [ "$(sha256sum "$incoming_deb" | cut -d' ' -f1)" = "$deb_sha" ] ||
            fail "install: cached .deb sha256 mismatch"

        # Prepare all backups before APT can overwrite legacy payloads. Publish them
        # after install succeeds, before removing shadowing files or restarting.
        new_previous="$run_dir/previous.env"
        legacy_dir="$run_dir/legacy"
        if [ -n "$installed_version" ]; then
            if [ "$current_sha" != "$deb_sha" ]; then
                install -m 0600 -- "$current_env" "$new_previous"
            fi
        else
            for src in "${{legacy_files[@]}}"; do
                if [ -e "$src" ] || [ -L "$src" ]; then
                    [[ -f "$src" && ! -L "$src" ]] ||
                        fail "legacy path must be a regular file before migration: $src"
                    mkdir -p -- "$legacy_dir"
                    cp -p -- "$src" "$legacy_dir/"
                fi
            done
            if [ -f "$legacy_dir/${{legacy_unit##*/}}" ]; then
                legacy_exec="$(legacy_executable "$legacy_unit")"
                [[ -f "$legacy_exec" && -x "$legacy_exec" ]] ||
                    fail "legacy helper executable missing or not executable; restore it before deploying"
                if [ "$legacy_exec" -ef "$packaged_helper" ]; then
                    [[ "$legacy_exec" = "$packaged_helper" && ! -L "$packaged_helper" ]] ||
                        fail "legacy helper aliases the packaged helper path; migrate it manually"
                    cp -p -- "$packaged_helper" "$legacy_dir/legacy-helper"
                else
                    dpkg-deb --fsys-tarfile "$incoming_deb" |
                        python3 -I -c {shlex.quote(LEGACY_PACKAGE_OVERLAP_CHECK)} / "$legacy_exec"
                fi
            fi
            if [ -d "$legacy_dir" ]; then
                printf 'PREV_MODE=legacy\nLEGACY_DIR=%s\n' "$legacy_dir" > "$new_previous"
            else
                printf 'PREV_MODE=none\n' > "$new_previous"
            fi
        fi

        if ! env DEBIAN_FRONTEND=noninteractive apt-get install -y --reinstall "$incoming_deb"; then
            fail "install: apt-get install failed; deploy backups retained in $run_dir"
        fi
        read_installed_version
        [ "$installed_version" = "$version" ] ||
            fail "install: expected Debian version $version after APT; deploy backups retained in $run_dir"
        if [[ ! -e "$stored_deb" || ( "$repair_archive" = 1 &&
              "$(sha256sum "$stored_deb" | cut -d' ' -f1)" != "$deb_sha" ) ]]; then
            mv -fT -- "$incoming_deb" "$stored_deb"
        fi

        # Record the installed package before post-install work that may need rollback.
        printf 'PREV_MODE=deb\nVERSION=%s\nDEB_PATH=%s\nDEB_SHA256=%s\n' \
            "$version" "$stored_deb" "$deb_sha" > "$run_dir/current.env"
        mv -fT -- "$run_dir/current.env" "$current_env"
        if [ -f "$new_previous" ]; then
            mv -fT -- "$new_previous" "$prev_env"
            rm -f -- "$restart_pending"
        fi

        for legacy_bin in aa-wireless-receiver aa-wired-receiver aa-wired-enrol aa-dummy-display; do
            if [ -x "$AA_DEPLOY_OPERATOR_HOME/aa-build/bin/$legacy_bin" ]; then
                printf 'legacy_bin_detected=%s/aa-build/bin/%s (superseded by /usr/bin/%s)\n' \
                    "$AA_DEPLOY_OPERATOR_HOME" "$legacy_bin" "$legacy_bin" >> "$run_dir/migration.log"
            fi
        done
        for src in "$legacy_udev" "$legacy_unit" "$legacy_dbus"; do
            if [ -f "$legacy_dir/${{src##*/}}" ]; then
                rm -f -- "$src"
                printf 'migrated=%s\n' "$src" >> "$run_dir/migration.log"
            fi
        done
        if [ -f "$legacy_dir/${{legacy_udev##*/}}" ] && command -v udevadm >/dev/null 2>&1; then
            udevadm control --reload || true
        fi

        write_receiver_digest
        cat -- "$current_env" > "$run_dir/current.env"
        printf 'RECEIVER_SHA256=%s\n' "$digest" >> "$run_dir/current.env"
        mv -fT -- "$run_dir/current.env" "$current_env"
        systemctl daemon-reload
        if [ "$should_restart" = "1" ]; then
            systemctl restart {shlex.quote(HELPER_UNIT)}
        fi

        dpkg-query -W > "$run_dir/dpkg-query-after.txt"
        printf 'INSTALLED_VERSION=%s\nRECEIVER_SHA256=%s\nHELPER_RESTARTED=%s\n' \
            "$version" "$digest" "$should_restart"
    """), stage_dir=stage_dir)


def build_rollback_script(state_dir: str, run_dir: str) -> str:
    return locked_mutation(mutation_script(state_dir, run_dir) + textwrap.dedent(f"""\
        [ -f "$prev_env" ] || fail "rollback: no previous deployment recorded at $prev_env"
        prev_mode="$(state_field "$prev_env" PREV_MODE)"
        helper_needs_restart
        read_installed_version
        digest=""
        effective_restart="$should_restart"
        if [ -f "$restart_pending" ]; then
            effective_restart=1
        fi
        case "$prev_mode" in
            deb)
                prev_ver="$(state_field "$prev_env" VERSION)"
                prev_deb="$(state_field "$prev_env" DEB_PATH)"
                prev_sha="$(state_field "$prev_env" DEB_SHA256)"
                [[ -n "$prev_deb" && -f "$prev_deb" ]] ||
                    fail "rollback: recorded previous .deb missing: $prev_deb"
                [ "$(sha256sum "$prev_deb" | cut -d' ' -f1)" = "$prev_sha" ] ||
                    fail "rollback: previous .deb sha256 mismatch"
                if [ "$effective_restart" = 1 ]; then touch -- "$restart_pending"; fi
                env DEBIAN_FRONTEND=noninteractive apt-get install -y --reinstall --allow-downgrades "$prev_deb"
                read_installed_version
                [[ -n "$prev_ver" && "$installed_version" = "$prev_ver" ]] ||
                    fail "rollback: previous Debian version was not installed; recovery state retained"
                write_receiver_digest
                rolled_to="$prev_ver"
                ;;
            legacy|none)
                # Require current metadata before removing any installed package.
                # If an earlier rollback completed removal, resume restoration.
                if [ -n "$installed_version" ]; then
                    read_current_metadata
                fi
                legacy_dir=""
                saved_unit=""
                if [ "$prev_mode" = legacy ]; then
                    legacy_dir="$(state_field "$prev_env" LEGACY_DIR)"
                    legacy_dir="${{legacy_dir:-$state_dir/rollback/legacy}}"
                    [ -d "$legacy_dir" ] || fail "rollback: recorded legacy backup directory missing"
                    saved_unit="$legacy_dir/${{legacy_unit##*/}}"
                    if [ -f "$saved_unit" ]; then
                        legacy_exec="$(legacy_executable "$saved_unit")"
                        if [ "$legacy_exec" = "$packaged_helper" ]; then
                            [[ -f "$legacy_dir/legacy-helper" && -x "$legacy_dir/legacy-helper" ]] ||
                                fail "rollback: saved legacy helper executable missing; recover it before rollback"
                        else
                            [[ -f "$legacy_exec" && -x "$legacy_exec" && ! "$legacy_exec" -ef "$packaged_helper" ]] ||
                                fail "rollback: legacy helper executable missing or aliases the packaged helper"
                        fi
                    fi
                fi

                if [ "$effective_restart" = 1 ] && [ "$prev_mode" = legacy ]; then
                    touch -- "$restart_pending"
                fi
                if [ -n "$installed_version" ]; then
                    env DEBIAN_FRONTEND=noninteractive apt-get remove -y {shlex.quote(PACKAGE_NAME)} ||
                        fail "rollback: package removal failed; recovery state retained"
                    read_installed_version
                    [ -z "$installed_version" ] ||
                        fail "rollback: package removal failed; package still installed and recovery state retained"
                fi

                if [ -n "$saved_unit" ] && [ -f "$saved_unit" ]; then
                    if [ "$legacy_exec" = "$packaged_helper" ]; then
                        helper_dir="${{packaged_helper%/*}}"
                        if [ ! -d "$helper_dir" ]; then install -d -m 0755 -- "$helper_dir"; fi
                        cp -p --remove-destination -- "$legacy_dir/legacy-helper" "$legacy_exec"
                    fi
                    [[ -f "$legacy_exec" && -x "$legacy_exec" ]] ||
                        fail "rollback: legacy helper executable unavailable after package removal"
                else
                    effective_restart=0
                fi

                for dst in "${{legacy_files[@]}}"; do
                    if [ -n "$legacy_dir" ] && [ -f "$legacy_dir/${{dst##*/}}" ]; then
                        cp -p --remove-destination -- "$legacy_dir/${{dst##*/}}" "$dst"
                        chown root:root "$dst"
                    elif [ "$dst" = "$etc_sha" ]; then
                        rm -f -- "$dst"
                    fi
                done
                if [ -f "$etc_sha" ]; then
                    saved_digest="$(cat -- "$etc_sha")"
                    if [[ "$saved_digest" =~ ^[a-f0-9]{{64}}$ ]]; then digest="$saved_digest"; fi
                fi
                if command -v udevadm >/dev/null 2>&1; then
                    udevadm control --reload || true
                fi
                rolled_to="$prev_mode"
                ;;
            *) fail "rollback: unknown previous mode: $prev_mode" ;;
        esac

        systemctl daemon-reload
        if [ "$effective_restart" = "1" ]; then
            systemctl restart {shlex.quote(HELPER_UNIT)}
        fi
        dpkg-query -W > "$run_dir/dpkg-query-after.txt"

        if [ "$prev_mode" = deb ]; then
            install -m 0600 -- "$prev_env" "$run_dir/current.env"
            mv -fT -- "$run_dir/current.env" "$current_env"
        fi
        if [ "$prev_mode" != deb ]; then
            rm -f -- "$current_env"
        fi
        rm -f -- "$restart_pending" "$prev_env"
        printf 'ROLLED_BACK_TO=%s\nHELPER_RESTARTED=%s\n' "$rolled_to" "$effective_restart"
        if [ -n "$digest" ]; then
            printf 'RECEIVER_SHA256=%s\n' "$digest"
        fi
    """))


def verify_infotainment_unchanged(
    runner: Runner,
    infotainment_dir: str,
    run_dir: str,
    before_text: str,
) -> None:
    try:
        after_text = runner.ssh(
            "post-deploy infotainment tree snapshot",
            build_infotainment_snapshot_script(infotainment_dir),
        )
        runner.ssh(
            "record post-deploy infotainment snapshot",
            f"set -euo pipefail\nprintf '%s' {shlex.quote(after_text)} | "
            f"sudo -n tee {shlex.quote(run_dir)}/infotainment-after.txt >/dev/null\n"
            f"sudo -n chmod 0600 {shlex.quote(run_dir)}/infotainment-after.txt\n",
        )
        if runner.dry_run:
            return
        before_kv = parse_kv(before_text)
        after_kv = parse_kv(after_text)
        if before_kv != after_kv:
            raise DeployError(
                "infotainment tree modified during deployment: "
                f"before={before_kv} after={after_kv}"
            )
    except DeployError as error:
        raise DeployError(f"package and configuration changes completed. {error}") from error


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="aa-deploy.sh",
        description="Idempotent SSH install, upgrade, and rollback for android-auto-receiver.",
    )
    parser.add_argument("target", help="SSH target (e.g. zucchi@192.168.55.3)")
    parser.add_argument("deb", nargs="?", type=Path, help="Path to local arm64 .deb (install/upgrade only)")
    parser.add_argument(
        "--confirm-jurisdiction",
        metavar="CC",
        help="Explicit two-letter uppercase regulatory jurisdiction matching operator ap.conf (required for install/upgrade)",
    )
    action = parser.add_mutually_exclusive_group()
    action.add_argument("--rollback", action="store_true", help="Roll back to the previous recorded package or legacy state")
    action.add_argument(
        "--adopt-installed", metavar="SHA256",
        help="Recover current.env from the installed package and the cached .deb with this trusted SHA-256",
    )
    parser.add_argument("--dry-run", action="store_true", help="Print every command without running it")
    parser.add_argument("--min-free-mb", type=int, default=DEFAULT_MIN_FREE_MB, help="Minimum free disk in MiB (default: 512)")
    parser.add_argument("--state-dir", default=DEFAULT_STATE_DIR, help=f"Remote state directory (default: {DEFAULT_STATE_DIR})")
    parser.add_argument(
        "--infotainment-dir",
        default=DEFAULT_INFOTAINMENT_DIR,
        help=f"Remote infotainment tree path to verify unchanged (default: {DEFAULT_INFOTAINMENT_DIR})",
    )
    parser.add_argument("--ssh-opt", action="append", default=[], help="Extra option passed to ssh/scp (repeatable)")
    args = parser.parse_args(argv)

    if args.rollback:
        if args.deb is not None:
            parser.error("do not pass a .deb path with --rollback")
    elif args.adopt_installed is not None:
        if args.deb is not None or args.confirm_jurisdiction is not None:
            parser.error("--adopt-installed takes a SHA-256 only, with no .deb path or --confirm-jurisdiction")
        if not SHA256_RE.fullmatch(args.adopt_installed):
            parser.error("--adopt-installed needs a 64-character lowercase SHA-256")
    else:
        if args.deb is None:
            parser.error("a .deb path is required for install/upgrade")
        if not args.confirm_jurisdiction:
            parser.error("--confirm-jurisdiction CC is required for install/upgrade")

    if args.confirm_jurisdiction and (
        not JURISDICTION_RE.fullmatch(args.confirm_jurisdiction) or args.confirm_jurisdiction == "00"
    ):
        parser.error("--confirm-jurisdiction must be two uppercase ASCII letters (e.g. US)")
    if args.min_free_mb < 1:
        parser.error("--min-free-mb must be positive")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    runner = Runner(args.target, args.ssh_opt, args.dry_run)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    run_id = f"{timestamp}-{os.getpid()}"
    action = "adopt" if args.adopt_installed is not None else "rollback" if args.rollback else "deploy"
    run_dir = f"{args.state_dir}/runs/{run_id}-{action}"

    try:
        if args.adopt_installed is not None:
            runner.ssh("adoption preflight checks", build_recovery_preflight_script(args.state_dir))
            output = runner.ssh(
                "adopt installed package",
                build_adopt_script(args.state_dir, run_dir, args.adopt_installed),
            )
            if not args.dry_run:
                print(output.strip())
            return 0

        if args.rollback:
            # Minimal rollback preflight: arch, state dir present, no measurement in progress.
            runner.ssh(
                "rollback preflight checks",
                build_recovery_preflight_script(args.state_dir),
            )
            try:
                info_before = runner.ssh(
                    "pre-rollback infotainment tree snapshot (optional)",
                    build_infotainment_snapshot_script(args.infotainment_dir, optional=True),
                )
            except DeployError as error:
                print(f"aa-deploy: warning: infotainment snapshot failed: {error}", file=sys.stderr)
                info_before = "INFOTAINMENT_STATUS=unavailable\n"
            infotainment_status = parse_kv(info_before).get("INFOTAINMENT_STATUS")
            if infotainment_status == "missing":
                print(
                    "aa-deploy: warning: infotainment tree missing or not a git repository; "
                    "continuing rollback without infotainment invariance check",
                    file=sys.stderr,
                )
            runner.ssh(
                "record installed package set before rollback",
                build_record_before_script(args.state_dir, run_dir, info_before),
            )
            rb_out = runner.ssh(
                "rollback to previous package/config",
                build_rollback_script(args.state_dir, run_dir),
            )
            if args.dry_run or infotainment_status == "present":
                verify_infotainment_unchanged(runner, args.infotainment_dir, run_dir, info_before)
            if not args.dry_run:
                rb_kv = parse_kv(rb_out)
                parts = [
                    f"rollback complete: target={args.target}",
                    f"rolled_back_to={rb_kv.get('ROLLED_BACK_TO', '')}",
                ]
                if rb_kv.get("RECEIVER_SHA256"):
                    parts.append(f"receiver_sha256={rb_kv['RECEIVER_SHA256']}")
                parts.append(f"helper_restarted={rb_kv.get('HELPER_RESTARTED', '0')}")
                parts.append(f"run_dir={run_dir}")
                print(" ".join(parts))
            return 0

        deb_info = inspect_local_deb(args.deb.resolve())

        # 1. Preflight checks (arch, Ubuntu 24.04, free disk, ap.conf + jurisdiction, no measurement).
        runner.ssh(
            "preflight checks",
            build_preflight_script(args.confirm_jurisdiction, args.min_free_mb, args.state_dir),
        )

        # 2. Infotainment tree pre-snapshot (read-only).
        info_before = runner.ssh(
            "pre-deploy infotainment tree snapshot",
            build_infotainment_snapshot_script(args.infotainment_dir, optional=False),
        )

        # 3. Record installed package set (dpkg-query -W) BEFORE any package-manager activity.
        runner.ssh(
            "record installed package set",
            build_record_before_script(args.state_dir, run_dir, info_before),
        )

        # 4. Stage .deb into a private mktemp directory on the target (no predictable /tmp path TOCTOU).
        stage_dir_out = runner.ssh(
            "create private staging directory",
            "set -euo pipefail\nmktemp -d /tmp/aa-deploy-stage.XXXXXX\n",
        )
        stage_dir = stage_dir_out.strip() if not args.dry_run else "/tmp/aa-deploy-stage.XXXXXX"
        # mktemp output; the test harness prefixes a fake root, so allow plain leading components.
        if not re.fullmatch(r"(?:/[A-Za-z0-9_-][A-Za-z0-9._-]*)*/tmp/aa-deploy-stage\.[A-Za-z0-9]{6}", stage_dir):
            raise DeployError("failed to create remote staging directory")
        staged_deb = f"{stage_dir}/{PACKAGE_NAME}_{deb_info['Version']}_{EXPECTED_ARCH}.deb"
        runner.scp("copy .deb to target", args.deb.resolve(), staged_deb)

        # 5. Verify transferred SHA256 and Depends satisfiability (after dpkg-query -W, before any install).
        runner.ssh(
            "verify staged .deb and Depends satisfiability",
            build_depends_check_script(stage_dir, staged_deb, deb_info["SHA256"]),
        )

        # 6. Install package, migrate legacy config after install, update receiver digest, conditionally restart helper.
        inst_out = runner.ssh(
            "install package, migrate legacy config, update digest, restart helper if active/enabled",
            build_install_script(
                args.state_dir,
                run_dir,
                stage_dir,
                staged_deb,
                deb_info,
            ),
        )

        # 7. Prove infotainment tree is unmodified.
        verify_infotainment_unchanged(runner, args.infotainment_dir, run_dir, info_before)

        if not args.dry_run:
            inst_kv = parse_kv(inst_out)
            digest = inst_kv.get("RECEIVER_SHA256", "")
            if not SHA256_RE.fullmatch(digest):
                raise DeployError(f"unexpected receiver sha256 output: {digest!r}")
            print(
                f"deploy complete: target={args.target} "
                f"version={inst_kv.get('INSTALLED_VERSION', deb_info['Version'])} "
                f"receiver_sha256={digest} "
                f"helper_restarted={inst_kv.get('HELPER_RESTARTED', '0')} "
                f"run_dir={run_dir}"
            )
        return 0
    except DeployError as error:
        print(f"aa-deploy: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
