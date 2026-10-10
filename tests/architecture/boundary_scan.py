#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Project module include-boundary scanner (task 14).

Scans the real project module trees (`include/aa/**`, `src/**`) and reports
forbidden external includes. It enforces three documented rules:

1. No aa:: public header may include Qt/QML, GStreamer, BlueZ, NetworkManager,
   AASDK, protobuf or OpenSSL headers. The protocol surface
   (`include/aa/protocol/**`) is covered by the same rule and additionally may
   not include `aa/tls/**`, whose legacy task-8 headers expose OpenSSL.
2. `include/aa/tls/**` and `src/tls/**` may include OpenSSL and each other
   (task-8 TLS module); nothing else consumes them directly.
3. In `src/**`, external includes are legal only inside private adapter
   directories (path segment `adapter`, `adapters`, or `*_adapter`); external
   implementations belong there and nowhere else.

Include directives are matched with robust `#`/`include` whitespace handling
(`# include`, `#\\tinclude\\t<...>`, `#include<...>`). Comments and string/char
literals are tokenized away first so commented spellings do not false-positive
and literals cannot confuse the scan; a quoted `#include` target is a
directive, not a literal, so it survives the blanking for the directive
matcher. Known limitation: macro-indirected includes (`#define H <x>` +
`#include H`) are not resolved.

Usage: boundary_scan.py ROOT
Exit: 0 clean, 1 violations, 2 usage error.
"""
from __future__ import annotations

import re
import sys
from collections.abc import Iterator
from pathlib import Path, PurePosixPath

# Forbidden external include families (regexes on the include target).
FAMILIES: dict[str, re.Pattern[str]] = {
    "qt": re.compile(r"^(?:Q[A-Z]\w*|q[a-z]\w*\.h|Qt\w+/)"),
    "gstreamer": re.compile(r"^gst/"),
    "bluez": re.compile(r"^bluetooth/"),
    "networkmanager": re.compile(r"^(?:NetworkManager\.h$|libnm/|nm-\w)"),
    "aasdk": re.compile(r"^aasdk/"),
    "protobuf": re.compile(r"^google/protobuf/"),
    "openssl": re.compile(r"^openssl/"),
}

# Intra-project confinement: the TLS module's OpenSSL-exposing headers.
TLS_MODULE_INCLUDE = re.compile(r"^aa/tls/")

# Directives: optional leading whitespace, '#', whitespace, 'include',
# whitespace, then a <...> or "..." target.
INCLUDE_DIRECTIVE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')

# A pending `#include` keyword at the head of the (comment-blanked) line: the
# quote that follows opens an include target, not a string literal.
PENDING_INCLUDE = re.compile(r"^\s*#\s*include\s*$")

SOURCE_SUFFIXES = {".hpp", ".h", ".hh", ".hxx", ".cpp", ".cc", ".cxx", ".c++"}


def blank_comments_and_literals(text: str) -> str:
    """Return `text` with comments and string/char literals blanked out.

    Newlines are preserved so line numbers stay stable. This is a real
    tokenizer (states: code, line comment, block comment, string, char,
    include target), so a commented-out include is not a violation while a
    real directive written with any whitespace shape survives for the
    directive matcher. A quote opening an `#include "..."` target preserves
    its target verbatim; any other quote is an ordinary literal and is
    blanked as before.
    """
    out: list[str] = []
    i = 0
    n = len(text)
    state = "code"
    line_start = 0
    while i < n:
        if out and out[-1] == "\n":
            line_start = len(out)
        char = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state == "code":
            if char == "/" and nxt == "/":
                state = "line"
                out.append("  ")
                i += 2
                continue
            if char == "/" and nxt == "*":
                state = "block"
                out.append("  ")
                i += 2
                continue
            if char == '"':
                if PENDING_INCLUDE.match("".join(out[line_start:])):
                    state = "include_target"
                    out.append('"')
                else:
                    state = "string"
                    out.append(" ")
                i += 1
                continue
            if char == "'":
                state = "char"
                out.append(" ")
                i += 1
                continue
            out.append(char)
            i += 1
            continue
        if state == "include_target":
            if char == '"' or char == "\n":
                state = "code"
            out.append(char)
            i += 1
            continue
        if state == "line":
            if char == "\n":
                state = "code"
                out.append("\n")
            else:
                out.append(" ")
            i += 1
            continue
        if state == "block":
            if char == "*" and nxt == "/":
                state = "code"
                out.append("  ")
                i += 2
                continue
            out.append("\n" if char == "\n" else " ")
            i += 1
            continue
        # string or char literal
        quote = '"' if state == "string" else "'"
        if char == "\\" and nxt:
            out.append("  ")
            i += 2
            continue
        if char == quote:
            state = "code"
            out.append(" ")
            i += 1
            continue
        out.append("\n" if char == "\n" else " ")
        i += 1
    return "".join(out)


def zone_of(rel: PurePosixPath) -> str:
    parts = rel.parts
    if not parts:
        return "src_impl"
    if parts[0] == "include":
        if len(parts) >= 3 and parts[1] == "aa" and parts[2] == "protocol":
            return "protocol_public"
        if len(parts) >= 3 and parts[1] == "aa" and parts[2] == "tls":
            return "tls_public"
        return "module_public"
    if any(segment in {"adapter", "adapters"} or segment.endswith("_adapter")
           for segment in parts[:-1]):
        return "adapter"
    if len(parts) >= 2 and parts[1] == "tls":
        return "tls_impl"
    return "src_impl"


def allowed(zone: str, family: str | None) -> bool:
    """Whether an include target (external `family` or aa/tls) is legal in `zone`."""
    if family is None:
        return True
    if family == "aa_tls":
        return zone in {"tls_public", "tls_impl", "adapter"}
    if zone == "adapter":
        return True
    if zone == "tls_public" or zone == "tls_impl":
        return family == "openssl"
    return False


def classify_target(target: str) -> str | None:
    if TLS_MODULE_INCLUDE.match(target):
        return "aa_tls"
    for name, pattern in FAMILIES.items():
        if pattern.match(target):
            return name
    return None


def iter_module_files(root: Path) -> Iterator[tuple[PurePosixPath, Path]]:
    for rel_root in ("include/aa", "src"):
        base = root / rel_root
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield PurePosixPath(path.relative_to(root).as_posix()), path


def scan(root: Path) -> tuple[list[str], int, int]:
    """Return (violations, include-file count, src-file count) for `root`."""
    violations: list[str] = []
    counts = {"include": 0, "src": 0}
    for rel, path in iter_module_files(root):
        counts["include" if rel.parts[0] == "include" else "src"] += 1
        zone = zone_of(rel)
        text = blank_comments_and_literals(path.read_text(encoding="utf-8"))
        for lineno, line in enumerate(text.splitlines(), start=1):
            match = INCLUDE_DIRECTIVE.match(line)
            if not match:
                continue
            target = match.group(1)
            family = classify_target(target)
            if family is not None and not allowed(zone, family):
                violations.append(f"{rel}:{lineno}: forbidden[{family}]: <{target}>")
    return violations, counts["include"], counts["src"]


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {argv[0]} ROOT", file=sys.stderr)
        return 2
    root = Path(argv[1])
    if not root.is_dir():
        print(f"boundary_scan: not a directory: {root}", file=sys.stderr)
        return 2
    violations, include_count, src_count = scan(root)
    for violation in violations:
        print(violation)
    print(f"violations={len(violations)} include={include_count} src={src_count}")
    return 1 if violations else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
