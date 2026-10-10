# SPDX-License-Identifier: GPL-3.0-or-later
"""Live target readers for the acquisition route: dpkg/kernel/JetPack identity.

Only invoked in a real target window; the route and selection tests fake this seam.
Each reader is a bounded read-only query (dpkg-query / dpkg -L / dpkg -S / proc /
nv_tegra_release). Package enumeration is cached by the selection index, never
queried per header. Stdlib-only and bundle-safe.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

from .acquire_models import (PackageInfo, ProcessObservation, QUIET_NOTE, QuietBoundary,
                             TargetIdentity)
from .kernel import Blocked
from .acquire_proc import observe_processes


def _run(argv: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(argv, capture_output=True, text=True, timeout=10, check=False)


def _dpkg_version(name: str) -> str | None:
    try:
        out = _run(["dpkg-query", "-W", "-f=${Version}", name])
    except (OSError, subprocess.SubprocessError):
        return None
    return out.stdout.strip() or None


def dpkg_list_files(package: str) -> tuple[str, ...]:
    try:
        out = _run(["dpkg", "-L", package])
    except (OSError, subprocess.SubprocessError):
        return ()
    return tuple(line for line in out.stdout.splitlines() if line.startswith("/"))


def dpkg_query(package: str) -> PackageInfo | None:
    try:
        out = _run(["dpkg-query", "-W", "-f=${Version}\t${Architecture}\t${Source}", package])
    except (OSError, subprocess.SubprocessError):
        return None
    parts = out.stdout.strip().split("\t")
    if out.returncode != 0 or len(parts) != 3:
        return None
    version, arch, source = parts
    return PackageInfo(package, version, arch, source or package)


def dpkg_owner(path: str) -> PackageInfo | None:
    try:
        out = _run(["dpkg", "-S", path])
    except (OSError, subprocess.SubprocessError):
        return None
    if out.returncode != 0:
        return None
    name = out.stdout.split(": ", 1)[0].strip()
    return dpkg_query(name)


def live_identity() -> TargetIdentity:
    from pathlib import Path
    kernel: str | None = None
    try:
        kernel = Path("/proc/sys/kernel/osrelease").read_text(encoding="utf-8").strip()
    except OSError:
        kernel = None
    l4t: str | None = None
    try:
        l4t = Path("/etc/nv_tegra_release").read_text(encoding="utf-8").strip()
    except OSError:
        l4t = None
    summary = tuple((name, _dpkg_version(name) or "")
                    for name in ("nvidia-l4t-core", "nvidia-jetpack", "libc6"))
    return TargetIdentity(kernel, l4t, _dpkg_version("nvidia-jetpack"), summary)


def live_package_of(path: str) -> PackageInfo:
    info = dpkg_owner(path)
    if info is None:
        raise Blocked("dpkg_owner_unresolved", path)
    return info


def assess_quiet(before: ProcessObservation, after: ProcessObservation,
                 roots: tuple[Path, ...]) -> QuietBoundary:
    """A writer is relevant iff its cwd is under a protected root; else unproven."""
    root_bytes = tuple(os.path.normpath(os.fsencode(str(r))) for r in roots)

    def under_root(cwd: str | None) -> bool:
        if cwd is None:
            return False
        raw = os.path.normpath(os.fsencode(cwd))
        return any(raw == root or raw.startswith(root + b"/") for root in root_bytes)

    relevant = tuple(sorted({
        f"{pid}:{comm}" for view in (before, after) for pid, comm, cwd in view.processes
        if under_root(cwd)
    }))
    complete = all(view.complete and all(cwd is not None for _, _, cwd in view.processes)
                   for view in (before, after))
    proven = complete and not relevant
    status = "quiescent" if proven else "unproven-activity"
    return QuietBoundary(before, after, relevant, status, proven, QUIET_NOTE)
