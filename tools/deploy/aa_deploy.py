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
AP_CONF = f"{ETC_DIR}/ap.conf"
SESSION_UID = f"{ETC_DIR}/session.uid"
RECEIVER_SHA256 = f"{ETC_DIR}/receiver.sha256"
HELPER_UNIT = "aa-system-helper.service"
LEGACY_UDEV_RULE = "/etc/udev/rules.d/70-android-auto-receiver.rules"
LEGACY_HELPER_UNIT = "/etc/systemd/system/aa-system-helper.service"
LEGACY_DBUS_POLICY = "/etc/dbus-1/system.d/org.custom.AndroidAutoSystemHelper.conf"
JURISDICTION_RE = re.compile(r"^[A-Z]{2}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


class DeployError(RuntimeError):
    """Actionable deployment failure."""


class Runner:
    """Executes or prints local, ssh, and scp commands."""

    def __init__(self, target: str, ssh_opts: list[str], dry_run: bool) -> None:
        self.target = target
        self.ssh_opts = ["-o", "BatchMode=yes", *ssh_opts]
        self.dry_run = dry_run

    def ssh_cmd(self, remote_script: str) -> list[str]:
        return ["ssh", *self.ssh_opts, self.target, remote_script]

    def scp_cmd(self, local_path: Path, remote_path: str) -> list[str]:
        return ["scp", *self.ssh_opts, str(local_path), f"{self.target}:{remote_path}"]

    def ssh(self, label: str, remote_script: str, *, capture: bool = True) -> str:
        cmd = self.ssh_cmd(remote_script)
        if self.dry_run:
            print(f"[dry-run] # {label}")
            print(f"[dry-run] {shlex.join(cmd)}")
            return ""
        proc = subprocess.run(
            cmd,
            text=True,
            capture_output=capture,
            check=False,
        )
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
        proc = subprocess.run(cmd, text=True, capture_output=True, check=False)
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
        proc = subprocess.run(
            ["dpkg-deb", "--field", str(deb_path), field],
            text=True,
            capture_output=True,
            check=False,
        )
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
    if not fields["Version"]:
        raise DeployError(f"empty Version field in {deb_path}")
    fields["SHA256"] = sha256_file(deb_path)
    return fields


def remote_path_expr(path: str) -> str:
    """Quote a remote path while allowing a leading $HOME/ expansion."""
    if path == "$HOME":
        return '"$HOME"'
    if path.startswith("$HOME/"):
        return '"$HOME/"' + shlex.quote(path[len("$HOME/"):])
    return shlex.quote(path)


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
        sudo -n sh -c '
            set -eu
            ap_conf=$1
            session_uid=$2
            expected_cc=$3
            if [ ! -f "$ap_conf" ] || [ -L "$ap_conf" ]; then
                echo "preflight: missing or non-regular operator ap.conf" >&2
                exit 1
            fi
            if [ ! -f "$session_uid" ] || [ -L "$session_uid" ]; then
                echo "preflight: missing or non-regular operator session.uid" >&2
                exit 1
            fi
            # Validate ap.conf schema and jurisdiction-confirmed without ever printing its contents.
            line_count=0
            valid_line=""
            while IFS= read -r raw || [ -n "$raw" ]; do
                [ -z "$raw" ] && continue
                line_count=$((line_count + 1))
                valid_line="$raw"
            done < "$ap_conf"
            if [ "$line_count" -ne 1 ]; then
                echo "preflight: ap.conf must contain exactly one configuration line" >&2
                exit 1
            fi
            set -- $valid_line
            if [ "$#" -ne 5 ] && [ "$#" -ne 6 ]; then
                echo "preflight: ap.conf schema invalid (expected 5 or 6 fields)" >&2
                exit 1
            fi
            if [ "$1" != "v1" ]; then
                echo "preflight: ap.conf schema version invalid" >&2
                exit 1
            fi
            if [ "$4" != "jurisdiction-confirmed" ]; then
                echo "preflight: ap.conf does not have jurisdiction-confirmed" >&2
                exit 1
            fi
            case "$3" in
                [A-Z][A-Z]) ;;
                *) echo "preflight: ap.conf country code malformed" >&2; exit 1 ;;
            esac
            if [ -n "$expected_cc" ] && [ "$3" != "$expected_cc" ]; then
                echo "preflight: --confirm-jurisdiction does not match operator ap.conf" >&2
                exit 1
            fi
            case "$5" in
                ""|*[!0-9]*) echo "preflight: ap.conf port invalid" >&2; exit 1 ;;
            esac
            if [ "$5" -lt 1 ] || [ "$5" -gt 65535 ]; then
                echo "preflight: ap.conf port out of range" >&2
                exit 1
            fi
            if [ "$#" -eq 6 ]; then
                case "$6" in
                    36|40|44|48) ;;
                    *) echo "preflight: ap.conf channel invalid (expected 36, 40, 44 or 48)" >&2; exit 1 ;;
                esac
            fi
        ' _ {shlex.quote(AP_CONF)} {shlex.quote(SESSION_UID)} {q_jur}
        echo "preflight=ok"
    """)


def build_infotainment_snapshot_script(infotainment_dir: str) -> str:
    dir_expr = remote_path_expr(infotainment_dir)
    return textwrap.dedent(f"""\
        set -euo pipefail
        dir={dir_expr}
        if [ ! -d "$dir" ]; then
            echo "infotainment: directory not found: $dir" >&2
            exit 1
        fi
        head_sha="$(git -C "$dir" rev-parse HEAD)"
        status_sha="$(git -C "$dir" status --porcelain | sha256sum | cut -d' ' -f1)"
        tree_sha="$(
            cd "$dir" &&
            find . -path './.git' -prune -o -type f -print0 |
            LC_ALL=C sort -z |
            xargs -0 -r sha256sum |
            sha256sum | cut -d' ' -f1
        )"
        printf 'HEAD=%s\nSTATUS_SHA256=%s\nTREE_SHA256=%s\n' "$head_sha" "$status_sha" "$tree_sha"
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
        active="$(systemctl is-active {shlex.quote(HELPER_UNIT)} 2>/dev/null || true)"
        enabled="$(systemctl is-enabled {shlex.quote(HELPER_UNIT)} 2>/dev/null || true)"
        printf 'HELPER_ACTIVE=%s\nHELPER_ENABLED=%s\n' "$active" "$enabled"
    """)


def parse_kv(output: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in output.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            result[key.strip()] = value.strip()
    return result


def build_depends_check_script(staged_deb: str, expected_sha: str) -> str:
    q_deb = shlex.quote(staged_deb)
    q_sha = shlex.quote(expected_sha)
    return textwrap.dedent(f"""\
        set -euo pipefail
        actual_sha="$(sha256sum {q_deb} | cut -d' ' -f1)"
        if [ "$actual_sha" != {q_sha} ]; then
            rm -f {q_deb}
            echo "transfer: staged .deb sha256 mismatch" >&2
            exit 1
        fi
        if ! apt-get -s install {q_deb} >/dev/null 2>&1; then
            rm -f {q_deb}
            echo "preflight: package Depends not satisfiable on target" >&2
            exit 1
        fi
        echo "depends=ok"
    """)


def build_install_script(
    state_dir: str,
    run_dir: str,
    staged_deb: str,
    deb_info: dict[str, str],
    helper_was_active: bool,
    helper_was_enabled: bool,
) -> str:
    q_state = shlex.quote(state_dir)
    q_run = shlex.quote(run_dir)
    q_staged = shlex.quote(staged_deb)
    version = deb_info["Version"]
    deb_sha = deb_info["SHA256"]
    stored_deb = f"{state_dir}/packages/{PACKAGE_NAME}_{version}_{EXPECTED_ARCH}.deb"
    q_stored = shlex.quote(stored_deb)
    should_restart = "1" if (helper_was_active or helper_was_enabled) else "0"
    return textwrap.dedent(f"""\
        set -euo pipefail
        # 1. Store the verified .deb in the root-owned package cache for rollback.
        sudo -n install -m 0644 -o root -g root {q_staged} {q_stored}
        rm -f {q_staged}

        # 2. Record previous deployment state (without clobbering rollback target on idempotent reinstall).
        sudo -n sh -c '
            set -eu
            state_dir=$1
            run_dir=$2
            new_ver=$3
            new_sha=$4
            etc_sha=$5
            if [ -f "$state_dir/current.env" ]; then
                cur_sha="$(grep -E "^DEB_SHA256=" "$state_dir/current.env" | head -n1 | cut -d= -f2- || true)"
                if [ "$cur_sha" != "$new_sha" ]; then
                    cp -p "$state_dir/current.env" "$state_dir/previous.env"
                    chmod 0600 "$state_dir/previous.env"
                    if [ -f "$etc_sha" ] && [ ! -L "$etc_sha" ]; then
                        install -m 0600 -o root -g root "$etc_sha" "$state_dir/rollback/receiver.sha256.prev"
                    fi
                fi
            else
                install -d -m 0700 -o root -g root "$state_dir/rollback/legacy"
                printf "PREV_MODE=legacy\n" > "$state_dir/previous.env"
                chmod 0600 "$state_dir/previous.env"
                if [ -f "$etc_sha" ] && [ ! -L "$etc_sha" ]; then
                    install -m 0600 -o root -g root "$etc_sha" "$state_dir/rollback/legacy/receiver.sha256"
                fi
            fi
        ' _ {q_state} {q_run} {shlex.quote(version)} {shlex.quote(deb_sha)} {shlex.quote(RECEIVER_SHA256)}

        # 3. Legacy ad-hoc config migration (udev shadow rule, old unit path, duplicate dbus policy, ~/aa-build/bin).
        migration_log=""
        if [ -d "$HOME/aa-build/bin" ]; then
            for legacy_bin in aa-wireless-receiver aa-wired-receiver aa-wired-enrol aa-dummy-display; do
                if [ -x "$HOME/aa-build/bin/$legacy_bin" ]; then
                    migration_log="${{migration_log}}legacy_bin_detected=$HOME/aa-build/bin/$legacy_bin (superseded by /usr/bin/$legacy_bin)\n"
                fi
            done
        fi
        sudo -n sh -c '
            set -eu
            state_dir=$1
            legacy_dir="$state_dir/rollback/legacy"
            install -d -m 0700 -o root -g root "$legacy_dir"
            migrate_file() {{
                src=$1
                dst_name=$2
                if [ -f "$src" ] && [ ! -L "$src" ]; then
                    if [ ! -f "$legacy_dir/$dst_name" ]; then
                        install -m 0644 -o root -g root "$src" "$legacy_dir/$dst_name"
                    fi
                    rm -f "$src"
                    printf "migrated=%s\n" "$src"
                fi
            }}
            migrate_file {shlex.quote(LEGACY_UDEV_RULE)} 70-android-auto-receiver.rules
            migrate_file {shlex.quote(LEGACY_HELPER_UNIT)} aa-system-helper.service
            migrate_file {shlex.quote(LEGACY_DBUS_POLICY)} org.custom.AndroidAutoSystemHelper.conf
        ' _ {q_state} | while IFS= read -r line; do
            printf '%s\n' "$line" | sudo -n tee -a {q_run}/migration.log >/dev/null
        done
        if [ -n "$migration_log" ]; then
            printf '%b' "$migration_log" | sudo -n tee -a {q_run}/migration.log >/dev/null
        fi

        # 4. Install the Debian package.
        sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y {q_stored}

        # 5. Recompute receiver digest from the INSTALLED receiver path.
        if [ ! -f {shlex.quote(INSTALLED_RECEIVER)} ] || [ -L {shlex.quote(INSTALLED_RECEIVER)} ]; then
            echo "deploy: installed receiver missing at {INSTALLED_RECEIVER}" >&2
            exit 1
        fi
        digest="$(sha256sum {shlex.quote(INSTALLED_RECEIVER)} | cut -d' ' -f1)"
        printf '%s\n' "$digest" | sudo -n tee {shlex.quote(RECEIVER_SHA256)}.new >/dev/null
        sudo -n chmod 0600 {shlex.quote(RECEIVER_SHA256)}.new
        sudo -n chown root:root {shlex.quote(RECEIVER_SHA256)}.new
        sudo -n mv -f {shlex.quote(RECEIVER_SHA256)}.new {shlex.quote(RECEIVER_SHA256)}

        # 6. Reload systemd; restart helper ONLY if it was already active or enabled. Never enable anything new.
        sudo -n systemctl daemon-reload
        if [ {should_restart} = "1" ]; then
            sudo -n systemctl restart {shlex.quote(HELPER_UNIT)}
        fi

        # 7. Record post-install package set and current deployment metadata.
        dpkg-query -W | sudo -n tee {q_run}/dpkg-query-after.txt >/dev/null
        sudo -n chmod 0600 {q_run}/dpkg-query-after.txt
        sudo -n sh -c '
            set -eu
            state_dir=$1
            printf "PREV_MODE=deb\nVERSION=%s\nDEB_PATH=%s\nDEB_SHA256=%s\nRECEIVER_SHA256=%s\n" \
                "$2" "$3" "$4" "$5" > "$state_dir/current.env.new"
            chmod 0600 "$state_dir/current.env.new"
            mv -f "$state_dir/current.env.new" "$state_dir/current.env"
        ' _ {q_state} {shlex.quote(version)} {q_stored} {shlex.quote(deb_sha)} "$digest"

        printf 'INSTALLED_VERSION=%s\nRECEIVER_SHA256=%s\nHELPER_RESTARTED=%s\n' \
            {shlex.quote(version)} "$digest" {should_restart}
    """)


def build_rollback_script(
    state_dir: str,
    run_dir: str,
    helper_was_active: bool,
    helper_was_enabled: bool,
) -> str:
    q_state = shlex.quote(state_dir)
    q_run = shlex.quote(run_dir)
    should_restart = "1" if (helper_was_active or helper_was_enabled) else "0"
    return textwrap.dedent(f"""\
        set -euo pipefail
        prev_env={q_state}/previous.env
        if ! sudo -n test -f "$prev_env"; then
            echo "rollback: no previous deployment recorded at $prev_env" >&2
            exit 1
        fi
        prev_mode="$(sudo -n grep -E '^PREV_MODE=' "$prev_env" | head -n1 | cut -d= -f2- || true)"
        if [ "$prev_mode" = "deb" ]; then
            prev_ver="$(sudo -n grep -E '^VERSION=' "$prev_env" | head -n1 | cut -d= -f2-)"
            prev_deb="$(sudo -n grep -E '^DEB_PATH=' "$prev_env" | head -n1 | cut -d= -f2-)"
            prev_sha="$(sudo -n grep -E '^DEB_SHA256=' "$prev_env" | head -n1 | cut -d= -f2-)"
            if [ -z "$prev_deb" ] || ! sudo -n test -f "$prev_deb"; then
                echo "rollback: recorded previous .deb missing: $prev_deb" >&2
                exit 1
            fi
            actual_sha="$(sudo -n sha256sum "$prev_deb" | cut -d' ' -f1)"
            if [ "$actual_sha" != "$prev_sha" ]; then
                echo "rollback: previous .deb sha256 mismatch" >&2
                exit 1
            fi
            sudo -n env DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades "$prev_deb"
            digest="$(sha256sum {shlex.quote(INSTALLED_RECEIVER)} | cut -d' ' -f1)"
            printf '%s\n' "$digest" | sudo -n tee {shlex.quote(RECEIVER_SHA256)}.new >/dev/null
            sudo -n chmod 0600 {shlex.quote(RECEIVER_SHA256)}.new
            sudo -n chown root:root {shlex.quote(RECEIVER_SHA256)}.new
            sudo -n mv -f {shlex.quote(RECEIVER_SHA256)}.new {shlex.quote(RECEIVER_SHA256)}
            sudo -n cp -p "$prev_env" {q_state}/current.env
            sudo -n chmod 0600 {q_state}/current.env
            rolled_to="$prev_ver"
        elif [ "$prev_mode" = "legacy" ]; then
            sudo -n env DEBIAN_FRONTEND=noninteractive apt-get remove -y {shlex.quote(PACKAGE_NAME)} || true
            sudo -n sh -c '
                set -eu
                legacy_dir="$1/rollback/legacy"
                restore_file() {{
                    src="$legacy_dir/$1"
                    dst=$2
                    mode=$3
                    if [ -f "$src" ]; then
                        install -m "$mode" -o root -g root "$src" "$dst"
                    fi
                }}
                restore_file 70-android-auto-receiver.rules {shlex.quote(LEGACY_UDEV_RULE)} 0644
                restore_file aa-system-helper.service {shlex.quote(LEGACY_HELPER_UNIT)} 0644
                restore_file org.custom.AndroidAutoSystemHelper.conf {shlex.quote(LEGACY_DBUS_POLICY)} 0644
                restore_file receiver.sha256 {shlex.quote(RECEIVER_SHA256)} 0600
                rm -f "$1/current.env"
            ' _ {q_state}
            if command -v udevadm >/dev/null 2>&1; then
                sudo -n udevadm control --reload || true
            fi
            digest="$(sudo -n cat {shlex.quote(RECEIVER_SHA256)} 2>/dev/null || true)"
            rolled_to="legacy"
        else
            echo "rollback: unknown previous mode: $prev_mode" >&2
            exit 1
        fi

        sudo -n systemctl daemon-reload
        if [ {should_restart} = "1" ]; then
            sudo -n systemctl restart {shlex.quote(HELPER_UNIT)}
        fi

        dpkg-query -W | sudo -n tee {q_run}/dpkg-query-after.txt >/dev/null
        sudo -n chmod 0600 {q_run}/dpkg-query-after.txt
        printf 'ROLLED_BACK_TO=%s\nRECEIVER_SHA256=%s\nHELPER_RESTARTED=%s\n' \
            "$rolled_to" "$digest" {should_restart}
    """)


def verify_infotainment_unchanged(
    runner: Runner,
    infotainment_dir: str,
    run_dir: str,
    before_text: str,
) -> None:
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

    if args.confirm_jurisdiction and not JURISDICTION_RE.fullmatch(args.confirm_jurisdiction):
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
        deb_info: dict[str, str] | None = None
        if not args.rollback:
            deb_info = inspect_local_deb(args.deb.resolve())

        # 1. Preflight checks (arch, Ubuntu 24.04, free disk, ap.conf + jurisdiction, no measurement).
        runner.ssh(
            "preflight checks",
            build_preflight_script(args.confirm_jurisdiction, args.min_free_mb, args.state_dir),
        )

        # 2. Infotainment tree pre-snapshot (read-only).
        info_before = runner.ssh(
            "pre-deploy infotainment tree snapshot",
            build_infotainment_snapshot_script(args.infotainment_dir),
        )

        # 3. Record installed package set (dpkg-query -W) BEFORE any package-manager activity.
        before_out = runner.ssh(
            "record installed package set and helper state",
            build_record_before_script(args.state_dir, run_dir, info_before),
        )
        before_kv = parse_kv(before_out)
        helper_was_active = before_kv.get("HELPER_ACTIVE") == "active"
        helper_was_enabled = before_kv.get("HELPER_ENABLED") == "enabled"

        if args.rollback:
            rb_out = runner.ssh(
                "rollback to previous package/config",
                build_rollback_script(args.state_dir, run_dir, helper_was_active, helper_was_enabled),
            )
            verify_infotainment_unchanged(runner, args.infotainment_dir, run_dir, info_before)
            if not args.dry_run:
                rb_kv = parse_kv(rb_out)
                print(
                    f"rollback complete: target={args.target} "
                    f"rolled_back_to={rb_kv.get('ROLLED_BACK_TO', '')} "
                    f"receiver_sha256={rb_kv.get('RECEIVER_SHA256', '')} "
                    f"helper_restarted={rb_kv.get('HELPER_RESTARTED', '0')} "
                    f"run_dir={run_dir}"
                )
            return 0

        assert deb_info is not None
        staged_deb = f"/tmp/aa-deploy-{timestamp}-{PACKAGE_NAME}_{deb_info['Version']}_{EXPECTED_ARCH}.deb"
        runner.scp("copy .deb to target", args.deb.resolve(), staged_deb)

        # 4. Verify transferred SHA256 and Depends satisfiability (after dpkg-query -W, before any install).
        runner.ssh(
            "verify staged .deb and Depends satisfiability",
            build_depends_check_script(staged_deb, deb_info["SHA256"]),
        )

        # 5. Migrate legacy config, install package, update receiver digest, conditionally restart helper.
        inst_out = runner.ssh(
            "migrate legacy config, install package, update digest, restart helper if active/enabled",
            build_install_script(
                args.state_dir,
                run_dir,
                staged_deb,
                deb_info,
                helper_was_active,
                helper_was_enabled,
            ),
        )

        # 6. Prove infotainment tree is unmodified.
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
