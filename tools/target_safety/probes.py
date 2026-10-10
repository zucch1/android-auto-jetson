# SPDX-License-Identifier: GPL-3.0-or-later
"""Enumerated read-only Jetson prerequisite observations inside a guard window."""
from __future__ import annotations

import glob
import hashlib
import json
import os
import subprocess
import time
from dataclasses import asdict
from pathlib import Path
from typing import Final

from .coverage import (EXTERNAL, NAMESPACE_LINKS, NAMESPACE_TEXT, REPOSITORY, Scope, protect,
                       sqlite_absence_recheck)
from .inventory import inventory, inventory_digest
from .kernel import Blocked, Watch, monitor_capacities
from .resource import ResourceError
PACKAGES: Final = (
    "nvidia-l4t-core", "nvidia-jetpack", "nvidia-l4t-gstreamer", "libc6", "libc6-dev",
    "linux-libc-dev", "g++", "libstdc++6", "libstdc++*-dev", "libprotobuf*",
    "protobuf-compiler", "libboost*-dev", "libssl-dev", "libssl3*", "libusb-1.0*",
    "libgstreamer*", "libgst*-dev", "qtbase5-dev", "qt6-base-dev", "libqt5*", "libqt6*",
)
PATTERNS: Final = (
    "/usr/include/stdio.h", "/usr/include/aarch64-linux-gnu/bits/libc-header-start.h",
    "/usr/include/c++/*/vector", "/usr/include/aarch64-linux-gnu/c++/*/bits/c++config.h",
    "/usr/include/google/protobuf/message.h", "/usr/include/boost/version.hpp",
    "/usr/include/openssl/ssl.h", "/usr/include/libusb-1.0/libusb.h",
    "/usr/include/gstreamer-1.0/gst/gst.h", "/usr/include/glib-2.0/glib.h",
    "/usr/include/aarch64-linux-gnu/qt*/QtCore/qobject.h",
    "/usr/lib/aarch64-linux-gnu/libc.so", "/usr/lib/aarch64-linux-gnu/libstdc++.so.6",
    "/usr/lib/gcc/aarch64-linux-gnu/*/libstdc++.so", "/usr/lib/aarch64-linux-gnu/libprotobuf.so",
    "/usr/lib/aarch64-linux-gnu/libboost_system.so", "/usr/lib/aarch64-linux-gnu/libboost_log.so",
    "/usr/lib/aarch64-linux-gnu/libssl.so", "/usr/lib/aarch64-linux-gnu/libusb-1.0.so",
    "/usr/lib/aarch64-linux-gnu/libgstreamer-1.0.so", "/usr/lib/aarch64-linux-gnu/libgstapp-1.0.so",
    "/usr/lib/aarch64-linux-gnu/libQt*Core.so",
)


def emit(kind: str, data: str | int | list[str]) -> None:
    """Stream evidence with the original guard's event/data envelope."""
    print(json.dumps({"event": kind, "data": data, "monotonic_ns": time.monotonic_ns()}), flush=True)


def command(watch: Watch, argv: list[str]) -> subprocess.CompletedProcess[str]:
    """Use lock-free Git and a per-command timeout within the overall window."""
    watch.check()
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update(GIT_MASTER="1", GIT_OPTIONAL_LOCKS="0", LC_ALL="C",
               GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL="/dev/null")
    emit("command_start", argv)
    result = subprocess.run(argv, cwd=REPOSITORY, env=env, capture_output=True,
                            text=True, check=False, timeout=min(20, watch.deadline - time.monotonic()))
    emit("command_result", json.dumps({"argv": argv, "exit_status": result.returncode,
                                      "stdout": result.stdout, "stderr": result.stderr}))
    watch.check()
    return result


def git_inventory(watch: Watch) -> tuple[str, ...]:
    """Capture dirty status plus tracked, untracked and ignored path sets."""
    if not (REPOSITORY / ".git").is_dir() or (REPOSITORY / ".git").is_symlink():
        raise Blocked("unsupported_git_directory", str(REPOSITORY / ".git"))
    prefix = ["git", "--no-pager", "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false"]
    outputs: list[str] = []
    for args in (["status", "--porcelain=v1", "-z", "--untracked-files=all"],
                 ["ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                 ["ls-files", "-z", "--others", "--ignored", "--exclude-standard"]):
        result = command(watch, prefix + args)
        if result.returncode:
            raise Blocked("git_inventory_failed", str(result.returncode))
        outputs.append(result.stdout)
    return tuple(outputs)


def prerequisites(watch: Watch) -> None:
    """Observe identity, package availability and header/library content hashes."""
    for argv in (["uname", "-a"], ["dpkg", "--print-architecture"]):
        if command(watch, argv).returncode:
            raise Blocked("identity_command_failed", str(argv))
    for name in ("/etc/os-release", "/etc/nv_tegra_release", "/proc/device-tree/model",
                 "/proc/device-tree/compatible", "/proc/device-tree/nvidia,dtsfilename"):
        watch.check()
        path = Path(name)
        emit("identity_file", json.dumps({"path": name, "value": path.read_text(errors="replace")
                                         if path.is_file() else None}))
        watch.check()
    # Missing package patterns are an observed prerequisite gap, not a guard failure.
    command(watch, ["dpkg-query", "-W", "-f=${binary:Package}\t${Version}\t${Architecture}\t${db:Status-Abbrev}\n",
                    *PACKAGES])
    for pattern in PATTERNS:
        watch.check()
        matches = sorted(glob.glob(pattern))
        emit("build_input_paths", json.dumps({"pattern": pattern, "matches": matches}))
        for name in matches:
            hasher = hashlib.sha256()
            size = 0
            with Path(name).open("rb") as stream:
                while chunk := stream.read(1024 * 1024):
                    size += len(chunk)
                    if size > 512 * 1024 ** 2:
                        raise Blocked("build_input_byte_limit", name)
                    hasher.update(chunk)
                    watch.check()
            emit("build_input_sha256", json.dumps([name, hasher.hexdigest()]))


def main() -> int:
    """Report safety only after final inventory, drain and descriptor cleanup."""
    watch: Watch | None = None
    try:
        with Watch.session() as active:
            watch = active
            # Setup diagnostics only after session start, before protect; recorded
            # as diagnostics and never a capacity pass (actual watches still required).
            emit("kernel_monitor_capacity_diagnostics", json.dumps({
                "readings": monitor_capacities(active),
                "claim": "setup diagnostics only; not a capacity pass; "
                         "actual watch installation required; no kernel writes",
            }))
            scope = Scope(REPOSITORY, EXTERNAL, REPOSITORY / ".codegraph", namespace_only=True)
            covered = protect(active, scope)
            emit("coverage_established", json.dumps({"objects": len(covered.paths), "watches": len(active.paths),
                                                    "roots": [str(REPOSITORY), str(EXTERNAL)],
                                                    "namespace_only": [str(link) for link in NAMESPACE_LINKS],
                                                    "literal_link_text": NAMESPACE_TEXT,
                                                    "sqlite_absence": covered.sqlite_absence,
                                                    "referent_protected": False}))
            before = inventory(active, covered.paths)
            emit("protected_before", json.dumps([asdict(row) for row in before]))
            before_git = git_inventory(active)
            emit("protected_before_sha256", inventory_digest(before))
            prerequisites(active)
            after = inventory(active, covered.paths)
            emit("protected_after", json.dumps([asdict(row) for row in after]))
            after_git = git_inventory(active)
            if covered.sqlite_absence is not None:
                sqlite_absence_recheck(active, scope, covered.sqlite_absence)
            active.check()
            if before != after or before_git != after_git:
                raise Blocked("persistent_protected_change")
        emit("watch_closed", len(active.paths))
        emit("protected_no_detected_write_no_persistent_change", inventory_digest(after))
        return 0
    except (OSError, Blocked, ResourceError, subprocess.TimeoutExpired) as error:
        if watch is not None:
            emit("watch_cleanup", json.dumps({"closed": watch.closed, "watches": len(watch.paths),
                                             "violations": [asdict(event) for event in watch.violations]}))
        emit("guard_blocked", str(error))
        return 70
