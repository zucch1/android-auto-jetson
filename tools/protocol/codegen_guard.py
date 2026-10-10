#!/usr/bin/env python3
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/protocol/codegen_guard.py --generated DIR [--generated DIR] --protos DIR [--protos DIR]
"""Codegen guard (task 15): reject OAA overlay in AASDK generated output.

Scans AASDK protobuf codegen output (``*.pb.h`` / ``*.pb.cc``) and the proto
import closure that produced it. The pinned AASDK schema set is the
``aap_protobuf`` import root; the Open Android Auto reference schemas (the
``oaa`` import root, ``oaa.proto.*`` packages) are reference-only and must never
appear in generated output or in the codegen import closure. Violations are
named diagnostics on stdout; exit 0 clean, 1 on any violation, 2 on usage error.
"""
from __future__ import annotations

import argparse
import re
import sys
from collections.abc import Iterable, Iterator
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Final

ALLOWED_IMPORT_ROOTS: Final[tuple[str, ...]] = ("aap_protobuf/", "google/protobuf/")
ALLOWED_PACKAGE_ROOTS: Final[tuple[str, ...]] = ("aap_protobuf.", "aap_protobuf")
OAA_ROOT: Final[str] = "oaa/"
OAA_PACKAGE_MARKERS: Final[tuple[str, ...]] = ("oaa.proto.", "oaa::", "namespace oaa")

PROTO_SUFFIXES: Final[frozenset[str]] = frozenset({".proto"})
GENERATED_SUFFIXES: Final[frozenset[str]] = frozenset({".pb.h", ".pb.cc", ".pb.cpp"})

PROTO_IMPORT = re.compile(r'^\s*import\s+(?:public\s+|weak\s+)?"([^"]+)"\s*;')
PROTO_PACKAGE = re.compile(r"^\s*package\s+([A-Za-z_][\w.]*)\s*;")
INCLUDE_DIRECTIVE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
STDLIB_HEADER = re.compile(r"^[A-Za-z_]\w*(?:\.[a-z]+)?$")


@dataclass(frozen=True, slots=True)
class Violation:
    code: str
    path: str
    line: int
    detail: str

    def render(self) -> str:
        return f"{self.code} {self.path}:{self.line} {self.detail}"


@dataclass(frozen=True, slots=True)
class ScanCounts:
    generated_files: int
    proto_files: int
    import_roots: tuple[str, ...]


def rel_display(path: Path, root: Path) -> str:
    try:
        return PurePosixPath(path.relative_to(root).as_posix()).as_posix()
    except ValueError:
        return path.as_posix()


def iter_files(roots: Iterable[Path], suffixes: frozenset[str]) -> Iterator[tuple[Path, Path]]:
    for root in roots:
        if not root.is_dir():
            return
        for path in sorted(root.rglob("*")):
            if path.is_file() and any(path.name.endswith(suffix) for suffix in suffixes):
                yield root, path


def import_root(target: str) -> str:
    return target.split("/", 1)[0] + "/" if "/" in target else target


def check_proto_file(path: Path, display: str) -> list[Violation]:
    violations: list[Violation] = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        import_match = PROTO_IMPORT.match(line)
        if import_match is not None:
            target = import_match.group(1)
            if target.startswith(OAA_ROOT):
                violations.append(Violation("OAA_IMPORT_ROOT", display, lineno, target))
            elif not target.startswith(ALLOWED_IMPORT_ROOTS):
                violations.append(Violation("UNEXPECTED_IMPORT_ROOT", display, lineno, target))
            continue
        package_match = PROTO_PACKAGE.match(line)
        if package_match is not None:
            package = package_match.group(1)
            if package == "oaa" or package.startswith(("oaa.", "oaa::")):
                violations.append(Violation("OAA_PACKAGE_ROOT", display, lineno, package))
            elif not package.startswith(ALLOWED_PACKAGE_ROOTS):
                violations.append(Violation("UNEXPECTED_PACKAGE_ROOT", display, lineno, package))
    return violations


def check_generated_file(path: Path, display: str) -> list[Violation]:
    violations: list[Violation] = []
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        include_match = INCLUDE_DIRECTIVE.match(line)
        if include_match is not None:
            target = include_match.group(1)
            if target.startswith(OAA_ROOT):
                violations.append(Violation("OAA_OVERLAY_INCLUDE", display, lineno, target))
            elif "/" in target and not target.startswith(ALLOWED_IMPORT_ROOTS):
                violations.append(Violation("UNEXPECTED_IMPORT_ROOT", display, lineno, target))
            continue
        if OAA_ROOT in line:
            violations.append(Violation("OAA_OVERLAY_SOURCE", display, lineno, line.strip()[:40]))
            continue
        for marker in OAA_PACKAGE_MARKERS:
            index = line.find(marker)
            if index >= 0:
                violations.append(
                    Violation("OAA_PACKAGE_ROOT", display, lineno, line[index : index + 40].strip())
                )
                break
    return violations


def scan(generated_roots: list[Path], proto_roots: list[Path]) -> tuple[list[Violation], ScanCounts]:
    violations: list[Violation] = []
    import_roots: set[str] = set()
    generated_count = 0
    proto_count = 0
    for root, path in iter_files(generated_roots, GENERATED_SUFFIXES):
        generated_count += 1
        violations.extend(check_generated_file(path, rel_display(path, root)))
    for root, path in iter_files(proto_roots, PROTO_SUFFIXES):
        proto_count += 1
        for line in path.read_text(encoding="utf-8").splitlines():
            import_match = PROTO_IMPORT.match(line)
            if import_match is not None:
                import_roots.add(import_root(import_match.group(1)))
        violations.extend(check_proto_file(path, rel_display(path, root)))
    for root in generated_roots:
        if root.is_dir() and not any(
            r == root for r, _ in iter_files([root], GENERATED_SUFFIXES)
        ):
            violations.append(Violation("EMPTY_ROOT", root.as_posix(), 0, "generated"))
    for root in proto_roots:
        if root.is_dir() and not any(r == root for r, _ in iter_files([root], PROTO_SUFFIXES)):
            violations.append(Violation("EMPTY_ROOT", root.as_posix(), 0, "protos"))
    return violations, ScanCounts(generated_count, proto_count, tuple(sorted(import_roots)))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--generated", action="append", type=Path, default=[])
    parser.add_argument("--protos", action="append", type=Path, default=[])
    args = parser.parse_args(argv)
    generated: list[Path] = [p.resolve() for p in args.generated]
    protos: list[Path] = [p.resolve() for p in args.protos]
    if not generated and not protos:
        print("codegen-guard: usage error: at least one --generated or --protos root", file=sys.stderr)
        return 2
    missing = [p for p in (*generated, *protos) if not p.is_dir()]
    if missing:
        print(f"codegen-guard: usage error: not a directory: {missing[0]}", file=sys.stderr)
        return 2
    violations, counts = scan(generated, protos)
    for violation in violations:
        print(violation.render())
    roots = ",".join(counts.import_roots) if counts.import_roots else "none"
    if violations:
        print(f"protocol-codegen-guard: FAIL violations={len(violations)}")
        return 1
    print(
        f"protocol-codegen-guard: PASS generated={counts.generated_files} "
        f"proto={counts.proto_files} import-roots={roots}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
