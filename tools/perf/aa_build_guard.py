#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Qualification build guard: refuse to start a stage from an unoptimized build.

A wired qualification or soak run measures the deployed binaries themselves, so
the build type is part of the measurement. A Debug build is not a slower
measurement of the same code: on a known-good commit, Debug runs measured
173.3 ms and 142.9 ms A/V sync while the same commit measured 16.2, 30.1 and
21.5 ms optimized, and building only the projection widget's pixel conversion
unoptimized reproduced a 106.8 ms failure against the 50 ms gate. The
unoptimized conversion runs on the GUI thread and delays paint and the 5 ms
display pump.

The build type is read from the build directory's ``CMakeCache.txt``
(``CMAKE_BUILD_TYPE``), never inferred from a binary name. ``Release``,
``RelWithDebInfo`` and ``MinSizeRelWithDebInfo`` are optimized; ``Debug``, an
empty value, an unreadable cache and a missing build directory are refused with
a message naming the fix. ``allow_unoptimized`` (``--allow-unoptimized-build``)
is a debugging escape hatch only: a run it admits is not a qualification result.
No budget, gate value or measurement is changed by this module.
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path

CACHE_FILE = "CMakeCache.txt"
CACHE_KEY = "CMAKE_BUILD_TYPE:"
# Optimized build types, lowercased. CMake's RelWithDebInfo keeps -O2 and only
# adds symbols, so it is acceptable when a run needs a usable backtrace.
OPTIMIZED_BUILD_TYPES = frozenset({"release", "relwithdebinfo", "minsizerelwithdebinfo"})
# Stderr marker the shell launchers grep for to report a distinct BLOCKED
# reason instead of a generic collector failure.
REFUSAL_PREFIX = "error: unoptimized_build:"
BLOCKED_REASON = "unoptimized_build"
BUILD_HINT = ("qualification must use an optimized build: `cmake --preset host-release` on the "
              "Jetson (or `jetson-aarch64` for cross builds); RelWithDebInfo is acceptable when "
              "symbols are needed")
OVERRIDE_HINT = ("pass --allow-unoptimized-build only to debug the tooling; its runs are not "
                 "qualification results")
BUILD_DIR_HINT = ("pass --build-dir (or AA_BUILD_DIR) naming the build directory that produced "
                  "the receiver and display binaries")


class BuildGuardError(Exception):
    """A qualification stage was refused because its build is not known to be optimized."""


def cache_path(build_dir: str | os.PathLike[str]) -> Path:
    """``<build_dir>/CMakeCache.txt``, accepting the cache path itself too."""
    path = Path(build_dir).expanduser()
    return path if path.name == CACHE_FILE else path / CACHE_FILE


def build_type_from_cache(build_dir: str | os.PathLike[str]) -> str | None:
    """Recorded ``CMAKE_BUILD_TYPE``, or None when the cache is absent/unreadable.

    An empty value ("CMAKE_BUILD_TYPE:STRING=") is returned as an empty string:
    the cache is readable but records no build type, which is not optimized.
    """
    try:
        text = cache_path(build_dir).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for line in text.splitlines():
        name, separator, value = line.partition("=")
        if separator and name.strip().startswith(CACHE_KEY):
            return value.strip()
    return None


def is_optimized(build_type: str | None) -> bool:
    return build_type is not None and build_type.strip().lower() in OPTIMIZED_BUILD_TYPES


def describe_build(build_dir: str | os.PathLike[str] | None, build_type: str | None) -> str:
    """One-line record for the run report (the value the operator must copy)."""
    if build_dir is None or not str(build_dir).strip():
        return f"build: CMAKE_BUILD_TYPE={build_type or 'unknown'} (no build directory given)"
    return f"build: CMAKE_BUILD_TYPE={build_type if build_type else 'unset'} (from {cache_path(build_dir)})"


def _refusal(build_dir: str | os.PathLike[str] | None) -> str | None:
    """Why this build cannot start a qualification stage, or None when it can."""
    if build_dir is None or not str(build_dir).strip():
        return (f"no build directory given, so the build type cannot be verified; {BUILD_DIR_HINT}; "
                f"{OVERRIDE_HINT}")
    path = cache_path(build_dir)
    build_type = build_type_from_cache(build_dir)
    if build_type is None:
        return (f"{path} is missing or unreadable, so the build type cannot be verified; "
                f"{BUILD_DIR_HINT}; {OVERRIDE_HINT}")
    if not is_optimized(build_type):
        recorded = build_type if build_type else "unset"
        return (f"{path} records CMAKE_BUILD_TYPE={recorded}; {BUILD_HINT}; {OVERRIDE_HINT}")
    return None


def require_optimized_build(build_dir: str | os.PathLike[str] | None, *,
                            allow_unoptimized: bool = False) -> str | None:
    """Return the recorded build type, or raise ``BuildGuardError`` to refuse.

    With ``allow_unoptimized`` the stage is allowed and the reason is written to
    stderr as a warning, so an override can never be silent.
    """
    problem = _refusal(build_dir)
    build_type = build_type_from_cache(build_dir) if build_dir else None
    if problem is None:
        return build_type
    if allow_unoptimized:
        print(f"warning: {BLOCKED_REASON}: {problem}", file=sys.stderr)
        return build_type
    raise BuildGuardError(problem)


def _cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default=os.environ.get("AA_BUILD_DIR"),
                        help="build directory whose CMakeCache.txt produced the launched binaries")
    parser.add_argument("--allow-unoptimized-build", action="store_true",
                        help="debugging override: start anyway, never for a qualification result")
    args = parser.parse_args(argv)
    try:
        build_type = require_optimized_build(args.build_dir,
                                            allow_unoptimized=args.allow_unoptimized_build)
    except BuildGuardError as exc:
        print(f"{REFUSAL_PREFIX} {exc}", file=sys.stderr)
        return 2
    print(describe_build(args.build_dir, build_type), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
