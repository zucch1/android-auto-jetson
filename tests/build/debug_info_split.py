# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B -E tests/build/debug_info_split.py --cmake-module cmake/DebugInfoSplit.cmake [--binary build/host-dev/aa_host_smoke --expect-split 1 --expect-inline-dwarf 0 --objcopy /usr/bin/objcopy]
"""Regression test for cmake/DebugInfoSplit.cmake (#176, #177)."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import zlib


FIXTURE_C = """\
#include <stdio.h>

static int helper(int value) {
    return value + 1;
}

int main(void) {
    return helper(-1);
}
"""

FIXTURE_CMAKELISTS = """\
cmake_minimum_required(VERSION 3.20)
project(debug_info_split_fixture LANGUAGES C)
option(AA_ENABLE_ASAN "Instrument project-owned host code with AddressSanitizer" OFF)
if(DEFINED EXPECT_CROSSCOMPILING)
    if(EXPECT_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Expected CMAKE_CROSSCOMPILING=TRUE for cross fixture")
    elseif(NOT EXPECT_CROSSCOMPILING AND CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR "Expected CMAKE_CROSSCOMPILING=FALSE for native fixture")
    endif()
endif()
add_executable(aa_split_fixture main.c)
include("${AA_DEBUG_INFO_SPLIT_MODULE}")
aa_split_debug_info_project_targets()
"""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(
    cmd: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None
) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(
        cmd, cwd=cwd, env=env, capture_output=True, text=True, check=False, timeout=60
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed ({result.returncode}): {' '.join(cmd)}\n"
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )
    return result


def c_locale_env() -> dict[str, str]:
    env = dict(os.environ)
    env["LC_ALL"] = "C"
    return env


def section_names(binary: Path) -> set[str]:
    readelf = shutil.which("readelf")
    if readelf is None:
        raise RuntimeError("readelf is required")
    output = run([readelf, "-W", "-S", str(binary)], env=c_locale_env()).stdout
    names: set[str] = set()
    for line in output.splitlines():
        match = re.match(r"^\s*\[\s*\d+\]\s+(\S+)", line)
        if match:
            names.add(match.group(1))
    return names


def debuglink_info(binary: Path) -> tuple[str, int]:
    readelf = shutil.which("readelf")
    if readelf is None:
        raise RuntimeError("readelf is required")
    output = run(
        [readelf, "--debug-dump=no-follow-links", "--debug-dump=links", str(binary)],
        env=c_locale_env(),
    ).stdout
    name_match = re.search(r"^\s*Separate debug info file:\s+(\S+)\s*$", output, re.MULTILINE)
    crc_match = re.search(r"^\s*CRC value:\s+0x([0-9a-fA-F]+)\s*$", output, re.MULTILINE)
    if name_match is None or crc_match is None:
        raise AssertionError(f"missing .gnu_debuglink metadata in {binary}:\n{output}")
    return name_match.group(1), int(crc_match.group(1), 16)


def assert_split(binary: Path, label: str) -> None:
    require(binary.is_file(), f"{label}: missing executable {binary}")
    sections = section_names(binary)
    require(".gnu_debuglink" in sections, f"{label}: {binary} missing .gnu_debuglink")
    expected_sidecar = f"{binary.name}.debug"
    actual_link, link_crc = debuglink_info(binary)
    require(
        actual_link == expected_sidecar,
        f"{label}: expected .gnu_debuglink={expected_sidecar!r}, got {actual_link!r}",
    )
    leftover_dwarf = sorted(name for name in sections if name.startswith(".debug_"))
    require(
        not leftover_dwarf,
        f"{label}: {binary} still contains DWARF sections: {leftover_dwarf}",
    )
    require(
        ".symtab" in sections and ".dynsym" in sections,
        f"{label}: {binary} must keep .symtab and .dynsym for hardening checks",
    )
    sidecar = binary.with_name(expected_sidecar)
    require(sidecar.is_file(), f"{label}: missing sidecar {sidecar}")
    mode = sidecar.stat().st_mode
    require(
        (mode & 0o111) == 0,
        f"{label}: sidecar {sidecar} must not be executable (mode={oct(mode)})",
    )
    actual_crc = zlib.crc32(sidecar.read_bytes()) & 0xFFFFFFFF
    require(
        link_crc == actual_crc,
        f"{label}: .gnu_debuglink CRC 0x{link_crc:08x} != sidecar CRC 0x{actual_crc:08x}",
    )
    sidecar_sections = section_names(sidecar)
    require(
        ".debug_info" in sidecar_sections,
        f"{label}: sidecar {sidecar} missing .debug_info DWARF section",
    )
    print(
        f"PASS {label}: split ({binary.name} -> {expected_sidecar}, crc=0x{actual_crc:08x})",
        flush=True,
    )


def assert_unsplit(binary: Path, label: str, *, expect_inline_dwarf: bool) -> None:
    require(binary.is_file(), f"{label}: missing executable {binary}")
    sections = section_names(binary)
    require(".gnu_debuglink" not in sections, f"{label}: {binary} unexpectedly has .gnu_debuglink")
    sidecar = binary.with_name(f"{binary.name}.debug")
    require(not sidecar.exists(), f"{label}: unexpected sidecar {sidecar}")
    if expect_inline_dwarf:
        require(".debug_info" in sections, f"{label}: {binary} missing inline .debug_info DWARF")
    print(
        f"PASS {label}: unsplit (inline_dwarf={'yes' if expect_inline_dwarf else 'not-required'})",
        flush=True,
    )


def build_fixture(
    *,
    cmake: str,
    generator: str,
    module: Path,
    source_dir: Path,
    build_dir: Path,
    build_type: str,
    asan: bool = False,
    cross: bool = False,
    split_option: bool | None = None,
) -> Path:
    args = [
        cmake,
        "-S", str(source_dir),
        "-B", str(build_dir),
        "-G", generator,
        f"-DAA_DEBUG_INFO_SPLIT_MODULE={module}",
        f"-DCMAKE_BUILD_TYPE={build_type}",
        f"-DAA_ENABLE_ASAN={'ON' if asan else 'OFF'}",
        f"-DEXPECT_CROSSCOMPILING={'ON' if cross else 'OFF'}",
    ]
    if split_option is not None:
        args.append(f"-DAA_SPLIT_DEBUG_INFO={'ON' if split_option else 'OFF'}")
    if cross:
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if compiler is None:
            raise RuntimeError("C compiler is required for synthetic cross fixture")
        toolchain = build_dir / "synthetic-cross-toolchain.cmake"
        build_dir.mkdir(parents=True, exist_ok=True)
        toolchain.write_text(
            "set(CMAKE_SYSTEM_NAME Linux)\n"
            "set(CMAKE_SYSTEM_PROCESSOR aarch64)\n"
            f"set(CMAKE_C_COMPILER \"{compiler}\")\n"
        )
        args.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")
    run(args)
    run([cmake, "--build", str(build_dir)])
    return build_dir / "aa_split_fixture"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake-module", type=Path, required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--expect-split", type=int, choices=(0, 1))
    parser.add_argument("--expect-inline-dwarf", type=int, choices=(0, 1), default=0)
    parser.add_argument("--objcopy")
    parser.add_argument("--cmake", default=shutil.which("cmake") or "cmake")
    parser.add_argument("--generator", default="Ninja")
    args = parser.parse_args()

    module = args.cmake_module.resolve()
    require(module.is_file(), f"missing DebugInfoSplit module: {module}")

    if args.binary is not None:
        if args.expect_split is None:
            raise SystemExit("--expect-split is required when --binary is given")
        if args.expect_split:
            if args.objcopy is not None:
                require(
                    bool(args.objcopy)
                    and not args.objcopy.endswith("-NOTFOUND")
                    and (Path(args.objcopy).is_file() or shutil.which(args.objcopy) is not None),
                    f"active-build:{args.binary.name}: split debug info is required for default "
                    f"host Debug/RelWithDebInfo builds, but CMAKE_OBJCOPY is unset or missing "
                    f"({args.objcopy!r})",
                )
            assert_split(args.binary, f"active-build:{args.binary.name}")
        else:
            assert_unsplit(
                args.binary,
                f"active-build:{args.binary.name}",
                expect_inline_dwarf=bool(args.expect_inline_dwarf),
            )

    with tempfile.TemporaryDirectory(prefix="aa-debug-split-") as temporary:
        root = Path(temporary)
        src = root / "src"
        src.mkdir()
        (src / "main.c").write_text(FIXTURE_C)
        (src / "CMakeLists.txt").write_text(FIXTURE_CMAKELISTS)

        # Leave AA_SPLIT_DEBUG_INFO unset so the default ON behavior is verified.
        debug_bin = build_fixture(
            cmake=args.cmake, generator=args.generator, module=module,
            source_dir=src, build_dir=root / "host-debug", build_type="Debug",
        )
        assert_split(debug_bin, "matrix:host-debug-default")
        # Clean must remove the sibling .debug sidecar too (ADDITIONAL_CLEAN_FILES).
        run([args.cmake, "--build", str(root / "host-debug"), "--target", "clean"])
        require(
            not debug_bin.with_name(f"{debug_bin.name}.debug").exists(),
            "matrix:host-debug-default: clean target left .debug sidecar behind",
        )
        print("PASS matrix:host-debug-clean: .debug sidecar removed", flush=True)

        relwithdebinfo_bin = build_fixture(
            cmake=args.cmake, generator=args.generator, module=module,
            source_dir=src, build_dir=root / "host-relwithdebinfo", build_type="RelWithDebInfo",
        )
        assert_split(relwithdebinfo_bin, "matrix:host-relwithdebinfo-default")

        asan_bin = build_fixture(
            cmake=args.cmake, generator=args.generator, module=module,
            source_dir=src, build_dir=root / "host-asan", build_type="Debug", asan=True,
        )
        assert_unsplit(asan_bin, "matrix:host-asan", expect_inline_dwarf=True)

        cross_debug_bin = build_fixture(
            cmake=args.cmake, generator=args.generator, module=module,
            source_dir=src, build_dir=root / "cross-debug", build_type="Debug", cross=True,
        )
        assert_unsplit(cross_debug_bin, "matrix:cross-debug", expect_inline_dwarf=True)

        cross_release_bin = build_fixture(
            cmake=args.cmake, generator=args.generator, module=module,
            source_dir=src, build_dir=root / "cross-release", build_type="Release", cross=True,
        )
        assert_unsplit(cross_release_bin, "matrix:cross-release", expect_inline_dwarf=False)

        release_bin = build_fixture(
            cmake=args.cmake, generator=args.generator, module=module,
            source_dir=src, build_dir=root / "host-release", build_type="Release",
        )
        assert_unsplit(release_bin, "matrix:host-release", expect_inline_dwarf=False)

        disabled_bin = build_fixture(
            cmake=args.cmake, generator=args.generator, module=module,
            source_dir=src, build_dir=root / "split-disabled", build_type="Debug",
            split_option=False,
        )
        assert_unsplit(disabled_bin, "matrix:split-disabled", expect_inline_dwarf=True)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
