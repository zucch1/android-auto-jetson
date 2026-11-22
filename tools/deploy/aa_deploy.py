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


def build_rollback_preflight_script(state_dir: str) -> str:
    """Minimal preflight for --rollback: arch, state dir present, no measurement in progress."""
    q_state = shlex.quote(state_dir)
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
        if ! sudo -n test -d {q_state}; then
            echo "preflight: deploy state directory missing:" {q_state} >&2
            exit 1
        fi
        echo "rollback_preflight=ok"
    """)


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


# Serialize deploy and rollback even when callers choose different state directories.
def locked_mutation(script: str) -> str:
    return (
        'set -euo pipefail\n'
        'sudo -n flock --close /run/aa-deploy.lock env AA_DEPLOY_OPERATOR_HOME="$HOME" '
        "bash -s <<'AA_LOCKED_MUTATION'\n"
        + script
        + "AA_LOCKED_MUTATION\n"
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
            *) fail "package is in dpkg state $status; repair it before deploying or rolling back" ;;
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
        fail "missing or inconsistent deployment metadata at $current_env; recover the managed package state first"
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
        if [ -n "$installed_version" ]; then
            read_current_metadata
            if dpkg --compare-versions "$installed_version" eq "$version" && [ "$current_sha" != "$deb_sha" ]; then
                fail "Debian version $version is already installed with different bytes; build with a new version"
            fi
            [[ -f "$current_deb" && "$(sha256sum "$current_deb" | cut -d' ' -f1)" = "$current_sha" ]] ||
                fail "current deployment archive missing or corrupt; recover it before upgrading"
        elif [ -e "$current_env" ] || [ -e "$prev_env" ]; then
            fail "inconsistent deployment metadata at $state_dir; finish recovery before deploying"
        fi

        # Version-keyed archives are immutable, including those used by an older rollback record.
        if [ -e "$stored_deb" ] || [ -L "$stored_deb" ]; then
            [[ -f "$stored_deb" && ! -L "$stored_deb" &&
                "$(sha256sum "$stored_deb" | cut -d' ' -f1)" = "$deb_sha" ]] ||
                fail "Debian version $version is already cached with different bytes; build with a new version"
        fi

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
                fi
            fi
            if [ -d "$legacy_dir" ]; then
                printf 'PREV_MODE=legacy\nLEGACY_DIR=%s\n' "$legacy_dir" > "$new_previous"
            else
                printf 'PREV_MODE=none\n' > "$new_previous"
            fi
        fi

        install -m 0644 -o root -g root {shlex.quote(staged_deb)} "$incoming_deb"
        [ "$(sha256sum "$incoming_deb" | cut -d' ' -f1)" = "$deb_sha" ] ||
            fail "install: cached .deb sha256 mismatch"
        if ! env DEBIAN_FRONTEND=noninteractive apt-get install -y "$incoming_deb"; then
            fail "install: apt-get install failed; deploy backups retained in $run_dir"
        fi
        read_installed_version
        [ "$installed_version" = "$version" ] ||
            fail "install: expected Debian version $version after APT; deploy backups retained in $run_dir"
        if [ ! -e "$stored_deb" ]; then
            mv -T -- "$incoming_deb" "$stored_deb"
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
    """))


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
                env DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades "$prev_deb"
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
    parser.add_argument("deb", nargs="?", type=Path, help="Path to local arm64 .deb (omit with --rollback)")
    parser.add_argument(
        "--confirm-jurisdiction",
        metavar="CC",
        help="Explicit two-letter uppercase regulatory jurisdiction matching operator ap.conf (required for install/upgrade)",
    )
    parser.add_argument("--rollback", action="store_true", help="Roll back to the previous recorded package or legacy state")
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
    else:
        if args.deb is None:
            parser.error("a .deb path is required unless --rollback is given")
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
    action = "rollback" if args.rollback else "deploy"
    run_dir = f"{args.state_dir}/runs/{run_id}-{action}"

    try:
        if args.rollback:
            # Minimal rollback preflight: arch, state dir present, no measurement in progress.
            runner.ssh(
                "rollback preflight checks",
                build_rollback_preflight_script(args.state_dir),
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
        if not stage_dir:
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
