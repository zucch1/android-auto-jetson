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
import re
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


_SHA256_RE = re.compile(r"^[0-9a-f]{64}$")
_SHA256SUM_LINE_RE = re.compile(r"^([0-9a-f]{64}) [ *](.+)$")


def is_sha256(value: object) -> bool:
    return isinstance(value, str) and _SHA256_RE.fullmatch(value) is not None


def derive_binaries_in_build_dir(build_dir: str | os.PathLike[str] | None,
                                 expected: Mapping[str, str]) -> dict[str, dict]:
    """Cross build: derive each launched binary and its SHA-256 from the local build tree.

    Used where the build tree is on the host that built it, so the build
    directory proves it produced a readable executable with the launched name and
    supplies its SHA-256 for pre-launch comparison against the remote target.
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
        build_path = candidates[0]
        digest = _sha256(Path(os.path.realpath(str(build_path))))
        if digest is None:
            problems.append(f"{role} {build_path} in {root} cannot be read, so its SHA-256 "
                            f"cannot be verified")
            continue
        records[role] = {"launched": name, "build_path": str(build_path),
                         "local_sha256": digest, "verified": "derived_from_build_dir"}
    if problems:
        raise BuildDirMismatchError("; ".join(problems) + f"; {BINARY_HINT}")
    return records


def parse_sha256sum_output(text: str) -> dict[str, str]:
    """Parse GNU sha256sum output strictly; fail closed on malformed or empty output."""
    hashes: dict[str, str] = {}
    for raw_line in text.splitlines():
        if not raw_line:
            continue
        match = _SHA256SUM_LINE_RE.fullmatch(raw_line)
        if match is None:
            raise BuildDirMismatchError(f"malformed remote sha256sum line: {raw_line!r}; {BINARY_HINT}")
        digest, path = match.groups()
        if path in hashes and hashes[path] != digest:
            raise BuildDirMismatchError(f"conflicting remote sha256sum entries for {path}; {BINARY_HINT}")
        hashes[path] = digest
    if not hashes:
        raise BuildDirMismatchError(f"remote sha256sum returned no hashes; {BINARY_HINT}")
    return hashes


def verify_remote_hashes(
    derived: Mapping[str, Mapping[str, object]],
    remote_paths: Mapping[str, str],
    sha256sum_output: str,
) -> dict[str, dict]:
    """Cross build (host side): match local build SHA-256s against remote sha256sum output."""
    if not remote_paths:
        raise BuildDirMismatchError(f"no remote binaries specified for SHA-256 verification; {BINARY_HINT}")
    remote_hashes = parse_sha256sum_output(sha256sum_output)
    expected_remote_paths = set(remote_paths.values())
    if set(remote_hashes) != expected_remote_paths:
        missing = sorted(expected_remote_paths - set(remote_hashes))
        extra = sorted(set(remote_hashes) - expected_remote_paths)
        details: list[str] = []
        if missing:
            details.append("missing remote hash(es) for " + ", ".join(missing))
        if extra:
            details.append("unexpected remote hash(es) for " + ", ".join(extra))
        raise BuildDirMismatchError("; ".join(details) + f"; {BINARY_HINT}")
    records: dict[str, dict] = {}
    problems: list[str] = []
    for role in sorted(remote_paths):
        remote_path = remote_paths[role]
        remote_sha256 = remote_hashes.get(remote_path)
        if not is_sha256(remote_sha256):
            problems.append(f"remote {role} {remote_path} has no valid SHA-256")
            continue
        local_entry = derived.get(role)
        if not isinstance(local_entry, Mapping):
            problems.append(f"local build output for {role} was not derived")
            continue
        local_sha256 = local_entry.get("local_sha256")
        build_path = local_entry.get("build_path")
        if not is_sha256(local_sha256) or not isinstance(build_path, str) or not build_path:
            problems.append(f"local {role} build record lacks a valid SHA-256")
            continue
        if remote_sha256 != local_sha256:
            problems.append(
                f"{role} {remote_path} (sha256:{remote_sha256}) does not match local build "
                f"output {build_path} (sha256:{local_sha256})"
            )
            continue
        records[role] = {
            "launched": remote_path,
            "build_path": build_path,
            "local_sha256": local_sha256,
            "remote_sha256": remote_sha256,
            "verified": "remote_sha256",
        }
    if problems:
        raise BuildDirMismatchError("; ".join(problems) + f"; {BINARY_HINT}")
    return records


def verify_expected_binary_hashes(
    binaries: Mapping[str, str | None],
    expected_hashes: Mapping[str, str],
    build_paths: Mapping[str, str] | None = None,
) -> dict[str, dict]:
    """Cross build (target side): verify each launched binary against its expected local SHA-256."""
    records: dict[str, dict] = {}
    problems: list[str] = []
    for role in sorted(binaries):
        path = binaries[role]
        if path is None or not str(path).strip():
            continue
        expected = expected_hashes.get(role)
        if not is_sha256(expected):
            problems.append(f"no valid expected SHA-256 provided for {role} ({path})")
            continue
        launched = os.path.realpath(str(path))
        launched_path = Path(launched)
        if not launched_path.is_file():
            problems.append(f"{role} {path} is missing or not a regular file")
            continue
        if not os.access(launched, os.X_OK):
            problems.append(f"{role} {path} is not executable")
            continue
        actual = _sha256(launched_path)
        if actual is None:
            problems.append(f"{role} {path} cannot be read, so its SHA-256 cannot be verified")
            continue
        if actual != expected:
            problems.append(
                f"{role} {path} (sha256:{actual}) does not match local build output "
                f"(sha256:{expected})"
            )
            continue
        entry: dict[str, str] = {
            "launched": str(path),
            "local_sha256": expected,
            "remote_sha256": actual,
            "verified": "remote_sha256",
        }
        if build_paths and role in build_paths:
            entry["build_path"] = build_paths[role]
        records[role] = entry
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
    parser.add_argument("--verify-remote", default=None, metavar="BUILD_CHECK_JSON",
                        help="verify remote sha256sum output against local hashes from --json output")
    parser.add_argument("--remote-sha256-file", default=None, metavar="PATH",
                        help="file containing stdout from remote sha256sum")
    parser.add_argument("--remote-binary", action="append", default=[], metavar="ROLE=REMOTE_PATH",
                        help="exact remote executable path for a role (with --verify-remote)")
    parser.add_argument("--json", action="store_true",
                        help="write the verified build record to stdout")
    args = parser.parse_args(argv)
    try:
        launched = parse_role_pairs(args.binary, "--binary")
        expected = parse_role_pairs(args.expect, "--expect")
        remote_binaries = parse_role_pairs(args.remote_binary, "--remote-binary")
    except ValueError as exc:
        print(f"usage error: {exc}", file=sys.stderr)
        return 2
    if args.verify_remote is not None:
        if not args.remote_sha256_file:
            print("usage error: --verify-remote requires --remote-sha256-file", file=sys.stderr)
            return 2
        try:
            local_doc = json.loads(Path(args.verify_remote).read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as exc:
            print(f"{BINARY_REFUSAL_PREFIX} cannot read local build record {args.verify_remote}: {exc}",
                  file=sys.stderr)
            return 2
        try:
            sha256_text = Path(args.remote_sha256_file).read_text(encoding="utf-8", errors="replace")
        except OSError as exc:
            print(f"{BINARY_REFUSAL_PREFIX} cannot read remote sha256sum output "
                  f"{args.remote_sha256_file}: {exc}", file=sys.stderr)
            return 2
        derived = local_doc.get("binaries") if isinstance(local_doc, dict) else None
        if not isinstance(derived, Mapping):
            print(f"{BINARY_REFUSAL_PREFIX} local build record has no derived binaries", file=sys.stderr)
            return 2
        try:
            verified = verify_remote_hashes(derived, remote_binaries, sha256_text)
        except BuildDirMismatchError as exc:
            print(f"{BINARY_REFUSAL_PREFIX} {exc}", file=sys.stderr)
            return 2
        for role, entry in sorted(verified.items()):
            print(f"build binary: {role} {entry['verified']} {entry['launched']} "
                  f"local_sha256={entry['local_sha256']} remote_sha256={entry['remote_sha256']}",
                  file=sys.stderr)
        if args.json:
            json.dump({"binaries": verified}, sys.stdout, indent=2, sort_keys=True)
            sys.stdout.write("\n")
        return 0
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
        sha_note = f" local_sha256={entry['local_sha256']}" if "local_sha256" in entry else ""
        print(f"build binary: {role} {entry['verified']} {entry['build_path']}{sha_note}", file=sys.stderr)
    if args.json:
        json.dump(record, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
    return 0


def parse_role_pairs(values: list[str], flag: str) -> dict[str, str]:
    pairs: dict[str, str] = {}
    for value in values:
        role, separator, name = value.partition("=")
        key = role.strip()
        val = name.strip()
        if not separator or not key or not val:
            raise ValueError(f"{flag} expects ROLE=VALUE, got {value!r}")
        if key in pairs:
            raise ValueError(f"{flag} duplicate role {key!r}")
        pairs[key] = val
    return pairs


_pairs = parse_role_pairs


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
