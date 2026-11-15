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
``RelWithDebInfo`` and ``MinSizeRel`` are optimized (case-insensitively);
``Debug``, an empty value, an unreadable cache and a missing build directory are
refused with a message naming the fix. ``allow_unoptimized``
(``--allow-unoptimized-build``) is a debugging escape hatch only: a run it
admits is not a qualification result.

Naming a build directory is not enough on its own: the guard also ties each
*launched* binary to that build. A binary inside the build directory (after
symlinks are resolved) is accepted; one deployed elsewhere is accepted only when
it is byte-identical to a binary that build produced; anything else is refused as
``build_dir_mismatch``, so another build's binaries can never run under this
build's record. For a cross build the tree lives on the host that built it, so
the host verifies the same cache and derives each binary from the build
directory (``--expect``); the target then records that the verification happened
on the host instead of re-reading a path it cannot see.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from collections.abc import Mapping
from pathlib import Path

CACHE_FILE = "CMakeCache.txt"
CACHE_KEY = "CMAKE_BUILD_TYPE:"
# Optimized build types, lowercased. CMake's RelWithDebInfo keeps -O2 and only
# adds symbols, so it is acceptable when a run needs a usable backtrace;
# MinSizeRel keeps -Os.
OPTIMIZED_BUILD_TYPES = frozenset({"release", "relwithdebinfo", "minsizerel"})
# Stderr marker the shell launchers grep for to report a distinct BLOCKED
# reason instead of a generic collector failure.
REFUSAL_PREFIX = "error: unoptimized_build:"
BLOCKED_REASON = "unoptimized_build"
# A launched binary that this build directory did not produce is its own
# refusal: it is not a slow build, it is a different build.
BINARY_REFUSAL_PREFIX = "error: build_dir_mismatch:"
BUILD_HINT = ("qualification must use an optimized build: `cmake --preset host-release` on the "
              "Jetson (or `jetson-aarch64` for cross builds); RelWithDebInfo is acceptable when "
              "symbols are needed")
OVERRIDE_HINT = ("pass --allow-unoptimized-build only to debug the tooling; its runs are not "
                 "qualification results")
BUILD_DIR_HINT = ("pass --build-dir (or AA_BUILD_DIR) naming the build directory that produced "
                  "the receiver and display binaries")
BINARY_HINT = ("launch the receiver and display from that build directory, or let the host "
               "verify the deployed copies")


class BuildGuardError(Exception):
    """A qualification stage was refused because its build is not known to be optimized."""


class BuildDirMismatchError(Exception):
    """A launched binary was not produced by the checked build directory."""


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


def require_optimized_type(build_type: str | None, *, allow_unoptimized: bool = False,
                           source: str = "the launched binaries") -> str | None:
    """Check a build type the caller already read (host-verified cross builds)."""
    if is_optimized(build_type):
        return build_type
    recorded = build_type if build_type else "unset"
    problem = (f"the build type verified for {source} is {recorded}; {BUILD_HINT}; {OVERRIDE_HINT}")
    if allow_unoptimized:
        print(f"warning: {BLOCKED_REASON}: {problem}", file=sys.stderr)
        return build_type
    raise BuildGuardError(problem)


def build_root(build_dir: str | os.PathLike[str]) -> Path:
    """Real path of the build directory (symlinks resolved), never the cache file."""
    return Path(os.path.realpath(str(cache_path(build_dir).parent)))


def _within(root: str, candidate: str) -> bool:
    try:
        return os.path.commonpath([root, candidate]) == root
    except ValueError:
        return False


def _sha256(path: Path) -> str | None:
    digest = hashlib.sha256()
    try:
        with open(path, "rb") as handle:
            for block in iter(lambda: handle.read(1 << 20), b""):
                digest.update(block)
    except OSError:
        return None
    return digest.hexdigest()


def _built_executables(root: Path, name: str) -> list[Path]:
    """Executables called ``name`` inside the build tree, symlinks resolved in-tree.

    A symlink that escapes the build directory is not something this build
    produced, so it is skipped rather than followed outside the tree. The walk is
    literal (never a glob) and skips CMake's own object-tree directories, which
    hold no launched binaries.
    """
    found: list[Path] = []
    root_text = str(root)
    for directory, subdirectories, files in os.walk(root):
        subdirectories[:] = [entry for entry in subdirectories
                             if entry not in {"CMakeFiles", ".git", ".svn"}]
        if name not in files:
            continue
        path = Path(directory) / name
        if not path.is_file():
            continue
        resolved = os.path.realpath(str(path))
        if not _within(root_text, resolved) or not os.access(resolved, os.X_OK):
            continue
        found.append(path)
    found.sort(key=lambda candidate: (len(candidate.parts), str(candidate)))
    return found


def verify_launched_binaries(build_dir: str | os.PathLike[str] | None,
                             binaries: Mapping[str, str | None]) -> dict[str, dict]:
    """Tie each launched binary to ``build_dir``; raise ``BuildDirMismatchError``.

    ``binaries`` maps a role (``receiver``/``display``) to the path that will be
    launched. Each path must be inside the build directory (symlinks resolved) or
    byte-identical to a binary that build directory produced; nothing else may
    run under this build's record.
    """
    if build_dir is None or not str(build_dir).strip():
        raise BuildDirMismatchError(f"no build directory given, so the launched binaries cannot be "
                                    f"tied to a build; {BUILD_DIR_HINT}")
    root = build_root(build_dir)
    root_text = str(root)
    records: dict[str, dict] = {}
    problems: list[str] = []
    for role in sorted(binaries):
        path = binaries[role]
        if path is None or not str(path).strip():
            continue
        launched = os.path.realpath(str(path))
        if _within(root_text, launched):
            records[role] = {"launched": str(path), "build_path": launched,
                             "verified": "in_build_dir"}
            continue
        digest = _sha256(Path(launched))
        if digest is None:
            problems.append(f"{role} {path} cannot be read, so it cannot be tied to {root}")
            continue
        match = next((candidate for candidate in _built_executables(root, os.path.basename(launched))
                      if _sha256(Path(os.path.realpath(str(candidate)))) == digest), None)
        if match is None:
            problems.append(f"{role} {path} is outside {root} and matches no "
                            f"{os.path.basename(launched)} that build produced")
            continue
        records[role] = {"launched": str(path), "build_path": str(match),
                         "verified": "identical_copy"}
    if problems:
        raise BuildDirMismatchError("; ".join(problems) + f"; {BINARY_HINT}")
    return records


def derive_binaries_in_build_dir(build_dir: str | os.PathLike[str] | None,
                                 expected: Mapping[str, str]) -> dict[str, dict]:
    """Cross build: derive each launched binary from the build directory's own tree.

    Used where the build tree is not visible (the host that built it), so the
    build directory proves it produced a binary with the launched name.
    """
    if build_dir is None or not str(build_dir).strip():
        raise BuildDirMismatchError(f"no build directory given, so the launched binaries cannot be "
                                    f"derived; {BUILD_DIR_HINT}")
    root = build_root(build_dir)
    records: dict[str, dict] = {}
    problems: list[str] = []
    for role in sorted(expected):
        name = expected[role]
        candidates = _built_executables(root, name)
        if not candidates:
            problems.append(f"{root} contains no executable named {name} for {role}")
            continue
        records[role] = {"launched": name, "build_path": str(candidates[0]),
                         "verified": "derived_from_build_dir"}
    if problems:
        raise BuildDirMismatchError("; ".join(problems) + f"; {BINARY_HINT}")
    return records


def _cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build-dir", default=os.environ.get("AA_BUILD_DIR"),
                        help="build directory whose CMakeCache.txt produced the launched binaries")
    parser.add_argument("--allow-unoptimized-build", action="store_true",
                        help="debugging override: start anyway, never for a qualification result")
    parser.add_argument("--binary", action="append", default=[], metavar="ROLE=PATH",
                        help="a binary that will be launched; it must belong to this build")
    parser.add_argument("--expect", action="append", default=[], metavar="ROLE=NAME",
                        help="derive the launched binary from the build directory (cross builds)")
    parser.add_argument("--json", action="store_true",
                        help="write the verified build record to stdout")
    args = parser.parse_args(argv)
    try:
        launched = _pairs(args.binary, "--binary")
        expected = _pairs(args.expect, "--expect")
    except ValueError as exc:
        print(f"usage error: {exc}", file=sys.stderr)
        return 2
    try:
        build_type = require_optimized_build(args.build_dir,
                                            allow_unoptimized=args.allow_unoptimized_build)
    except BuildGuardError as exc:
        print(f"{REFUSAL_PREFIX} {exc}", file=sys.stderr)
        return 2
    record = {"build_type": build_type,
              "build_dir": str(args.build_dir) if args.build_dir else None,
              "allow_unoptimized_build": bool(args.allow_unoptimized_build)}
    try:
        binaries: dict[str, dict] = {}
        if launched:
            binaries.update(verify_launched_binaries(args.build_dir, launched))
        if expected:
            binaries.update(derive_binaries_in_build_dir(args.build_dir, expected))
    except BuildDirMismatchError as exc:
        print(f"{BINARY_REFUSAL_PREFIX} {exc}", file=sys.stderr)
        return 2
    if binaries:
        record["binaries"] = binaries
    print(describe_build(args.build_dir, build_type), file=sys.stderr)
    for role, entry in sorted(binaries.items()):
        print(f"build binary: {role} {entry['verified']} {entry['build_path']}", file=sys.stderr)
    if args.json:
        json.dump(record, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    return 0


def _pairs(values: list[str], flag: str) -> dict[str, str]:
    pairs: dict[str, str] = {}
    for value in values:
        role, separator, name = value.partition("=")
        if not separator or not role.strip() or not name.strip():
            raise ValueError(f"{flag} expects ROLE=VALUE, got {value!r}")
        pairs[role.strip()] = name.strip()
    return pairs


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
