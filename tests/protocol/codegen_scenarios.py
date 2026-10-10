#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/protocol/codegen_scenarios.py ROOT CASE GENERATED PROTOS
"""Codegen-guard scenarios (task 15).

The happy cases run the shipped guard over the real staged proto tree and the
real AASDK generated output. Every negative case writes a REAL temporary
overlay fixture (OAA import root, package, namespace, source comment, or an
unknown overlay root) and requires the guard to fail closed on it. Nothing is
asserted against a checked-in fixture string.
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Final

GUARD: Final[Path] = Path(__file__).resolve().parents[2] / "tools" / "protocol" / "codegen_guard.py"
MIN_REAL_GENERATED: Final[int] = 400
MIN_REAL_PROTOS: Final[int] = 200
PASS_SUMMARY = re.compile(r"generated=(\d+) proto=(\d+)")


def run_guard(*, generated: Path | None = None, protos: Path | None = None) -> subprocess.CompletedProcess[str]:
    args = [sys.executable, "-B", str(GUARD)]
    if generated is not None:
        args += ["--generated", str(generated)]
    if protos is not None:
        args += ["--protos", str(protos)]
    return subprocess.run(args, capture_output=True, text=True, check=False)


def expect_violation(result: subprocess.CompletedProcess[str], marker: str, case: str) -> int:
    if result.returncode != 1:
        print(f"{case}: expected guard exit 1, got {result.returncode}\n{result.stdout}{result.stderr}")
        return 1
    if marker not in result.stdout:
        print(f"{case}: expected {marker} in output\n{result.stdout}")
        return 1
    return 0


def expect_pass(result: subprocess.CompletedProcess[str], case: str, min_generated: int, min_protos: int) -> int:
    if result.returncode != 0:
        print(f"{case}: expected guard exit 0, got {result.returncode}\n{result.stdout}{result.stderr}")
        return 1
    summary = PASS_SUMMARY.search(result.stdout)
    if summary is None:
        print(f"{case}: missing PASS summary\n{result.stdout}")
        return 1
    generated, protos = (int(value) for value in summary.groups())
    if generated < min_generated or protos < min_protos:
        print(f"{case}: vacuous scan generated={generated} proto={protos}")
        return 1
    return 0


def fixture_root(case: str) -> Path:
    return Path(tempfile.mkdtemp(prefix=f"protocol-codegen-{case}-"))


def write_file(root: Path, rel: str, body: str) -> Path:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(body, encoding="utf-8")
    return root


def case_real_generated(_root: Path, generated: Path, _protos: Path) -> int:
    return expect_pass(run_guard(generated=generated), "real-generated", MIN_REAL_GENERATED, 0)


def case_real_protos(_root: Path, _generated: Path, protos: Path) -> int:
    return expect_pass(run_guard(protos=protos), "real-protos", 0, MIN_REAL_PROTOS)


NEGATIVE_CASES: Final[dict[str, tuple[str, str, str]]] = {
    "neg-oaa-include": (
        "generated",
        "x.pb.h",
        '#include "oaa/control/ControlChannelData.pb.h"\n',
    ),
    "neg-oaa-source": (
        "generated",
        "y.pb.cc",
        "// Source: oaa/control/ControlChannelData.proto\n",
    ),
    "neg-oaa-namespace": (
        "generated",
        "z.pb.h",
        "namespace oaa { namespace proto { namespace data {\n",
    ),
    "neg-unexpected-root": (
        "generated",
        "w.pb.h",
        '#include "vendor/evil/thing.pb.h"\n',
    ),
    "neg-oaa-import": (
        "protos",
        "overlay.proto",
        'syntax = "proto2";\nimport "oaa/control/ControlChannelData.proto";\n',
    ),
    "neg-oaa-package": (
        "protos",
        "overlay.proto",
        'syntax = "proto2";\npackage oaa.proto.messages;\n',
    ),
}


def make_negative(case: str) -> int:
    kind, rel, body = NEGATIVE_CASES[case]
    fixture = write_file(fixture_root(case), rel, body)
    result = run_guard(generated=fixture) if kind == "generated" else run_guard(protos=fixture)
    marker = {
        "neg-oaa-include": "OAA_OVERLAY_INCLUDE",
        "neg-oaa-source": "OAA_OVERLAY_SOURCE",
        "neg-oaa-namespace": "OAA_PACKAGE_ROOT",
        "neg-unexpected-root": "UNEXPECTED_IMPORT_ROOT",
        "neg-oaa-import": "OAA_IMPORT_ROOT",
        "neg-oaa-package": "OAA_PACKAGE_ROOT",
    }[case]
    return expect_violation(result, marker, case)


def case_empty_root(_root: Path, _generated: Path, _protos: Path) -> int:
    fixture = fixture_root("empty-root")
    return expect_violation(run_guard(generated=fixture), "EMPTY_ROOT", "empty-root")


CASES: Final[dict[str, Callable[[Path, Path, Path], int]]] = {
    "real-generated": case_real_generated,
    "real-protos": case_real_protos,
    "empty-root": case_empty_root,
}


def main(argv: list[str]) -> int:
    if len(argv) != 5:
        print(f"usage: {argv[0]} ROOT CASE GENERATED PROTOS", file=sys.stderr)
        return 2
    root = Path(argv[1]).resolve()
    case = argv[2]
    generated = Path(argv[3]).resolve()
    protos = Path(argv[4]).resolve()
    if case in NEGATIVE_CASES:
        return make_negative(case)
    if case in CASES:
        return CASES[case](root, generated, protos)
    print(f"unknown case: {case}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
