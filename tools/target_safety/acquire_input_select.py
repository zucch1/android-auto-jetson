# SPDX-License-Identifier: GPL-3.0-or-later
"""Frozen package-selection policy: complete C++/GoogleTest ARM64 build input closure.

Applies the frozen acquire-input-policy.json on the target: enumerate each
selected package once (dpkg -L + dpkg-query -W) into a cached ownership index --
never a subprocess per header -- take complete /usr/include and the necessary
ARM64 libs under the configured lib dirs, then close over symlink referents and
/lib linker-script GROUP/INPUT referents. Identity (L4T/dpkg/kernel) is observed
live. The exact resulting file list is captured at runtime, not hard-coded. The
dpkg/identity seam is readers.*; tests fake it against fixture trees.
"""
from __future__ import annotations

import json
import os
import re
import shlex
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .acquire_models import InputObservation, InputSpec, PackageInfo
from .acquire_readers import dpkg_list_files, dpkg_owner, dpkg_query, live_identity
from .kernel import Blocked

POLICY_PATH: Final = Path(__file__).resolve().parent / "acquire-input-policy.json"
_SCRIPT_RE: Final = re.compile(r"\b(?:GROUP|INPUT)\s*\(")


@dataclass(frozen=True, slots=True)
class Policy:
    package_rules: tuple[str, ...]
    header_roots: tuple[str, ...]
    lib_dirs: tuple[str, ...]
    linker_markers: tuple[str, ...]


def load_policy(path: Path = POLICY_PATH) -> Policy:
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict) or raw.get("schema") != "aa-acquire-input-policy/1":
        raise Blocked("policy_malformed", str(path))

    def strings(key: str) -> tuple[str, ...]:
        value = raw.get(key)
        if not isinstance(value, list) or not all(isinstance(v, str) for v in value):
            raise Blocked("policy_malformed", key)
        return tuple(value)
    return Policy(strings("package_rules"), strings("header_roots"),
                  strings("lib_dirs"), strings("linker_script_markers"))


def _under(path: str, roots: tuple[str, ...]) -> bool:
    return any(path == root or path.startswith(root + "/") for root in roots)


def _directory_path(path: str) -> str:
    # Normalize usrmerge/GCC directory aliases without following the owned leaf symlink.
    return os.path.join(os.path.realpath(os.path.dirname(path)), os.path.basename(path))


class PackageIndex:
    """Cached package -> files + path -> owner; one dpkg query per package."""

    def __init__(self, rules: tuple[str, ...]) -> None:
        self._by_path: dict[str, PackageInfo] = {}
        self._by_directory_path: dict[str, PackageInfo] = {}
        for package in rules:
            info = dpkg_query(package)
            if info is None:
                continue
            for file in dpkg_list_files(package):
                self._by_path[file] = info
                self._by_directory_path[_directory_path(file)] = info

    def owner(self, path: str) -> PackageInfo | None:
        return self._by_path.get(path) or self._by_directory_path.get(_directory_path(path))

    def paths(self) -> tuple[str, ...]:
        return tuple(self._by_path)


def _is_linker_script(path: str, policy: Policy) -> bool:
    try:
        with open(path, "rb") as stream:
            head = stream.read(4096)
    except OSError:
        return False
    text = head.decode("utf-8", "replace")
    return _SCRIPT_RE.search(re.sub(r"/\*.*?\*/", "", text, flags=re.S)) is not None


def _resolve_linker(text: str) -> set[str]:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    refs: set[str] = set()
    for match in _SCRIPT_RE.finditer(text):
        depth = 1
        end = match.end()
        while end < len(text) and depth:
            depth += (text[end] == "(") - (text[end] == ")")
            end += 1
        if depth:
            raise Blocked("linker_script_malformed", text)
        tokens = shlex.split(text[match.end():end - 1].replace("(", " ").replace(")", " "))
        refs.update(token for token in tokens if token not in ("AS_NEEDED", ","))
    return refs


def _symlink_referent(path: str) -> str | None:
    try:
        if not os.path.islink(path):
            return None
        target = os.readlink(path)
    except OSError:
        return None
    if not target.startswith("/"):
        target = str(Path(path).parent / target)
    return os.path.normpath(target)


def _closure(selected: set[str], policy: Policy) -> set[str]:
    frontier = set(selected)
    resolved = set(selected)
    while frontier:
        added: set[str] = set()
        for path in frontier:
            referent = _symlink_referent(path)
            if referent is not None and referent not in resolved:
                added.add(referent)
            if _is_linker_script(path, policy):
                for ref in _resolve_linker(Path(path).read_text(errors="replace")):
                    if not ref.startswith("/"):
                        # ld searches each directory for .so, then .a (-lgcc uses libgcc.a).
                        names = ("lib" + ref[2:] + ".so", "lib" + ref[2:] + ".a") if ref.startswith("-l") else (ref,)
                        candidates = [str(Path(d) / name)
                                      for d in (str(Path(path).parent), *policy.lib_dirs)
                                      for name in names]
                        ref = next((p for p in candidates if os.path.lexists(p)), candidates[0])
                    if ref not in resolved:
                        added.add(ref)
        frontier = added - resolved
        resolved |= added
    return resolved


def select_inputs(policy_path: Path = POLICY_PATH) -> InputObservation:
    policy = load_policy(policy_path)
    index = PackageIndex(policy.package_rules)
    selected = {path for path in index.paths()
                if (_under(path, policy.header_roots) or _under(path, policy.lib_dirs))
                and (stat.S_ISREG(os.lstat(path).st_mode) or stat.S_ISLNK(os.lstat(path).st_mode))}
    selected = _closure(selected, policy)
    packages: dict[str, PackageInfo] = {}
    for path in sorted(selected):
        owner = index.owner(path) or index.owner(os.path.realpath(path)) or dpkg_owner(path)
        if owner is None:
            raise Blocked("selection_owner_unresolved", path)
        packages[path] = owner
    return InputObservation(tuple(InputSpec(p) for p in sorted(selected)),
                            packages, live_identity())
