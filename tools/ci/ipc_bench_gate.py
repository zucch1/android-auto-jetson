# SPDX-License-Identifier: GPL-3.0-or-later
"""Decide whether a pull request must run the 600 s ipc-bench-happy-600 gate.

The gate exercises exactly one binary (aa-ipc-bench and its aa-ipc-transport
library) through one CTest registration. This compares, between two configured
source trees (the PR base and the PR merge result), everything that defines
what that gate runs:

  * the content of every file under tools/aa-ipc-bench/;
  * every build command ninja runs to produce aa-ipc-bench (compiler, flags,
    defines, include paths, link line, dependencies), with tree paths
    normalised;
  * the CTest registration of ipc-bench-happy-600 (command line and
    properties other than the CMake backtrace).

Identical fingerprints mean the PR cannot change the benchmark as built and
run, so the gate is deferred to main/nightly. Any difference, or any error,
means the gate runs. Exit status: 0 = defer, 1 = run, 2 = error (run).
"""
import argparse
import hashlib
import re
import subprocess
import sys
from pathlib import Path

BENCH_DIR = 'tools/aa-ipc-bench'
BENCH_TARGET = 'aa-ipc-bench'
BENCH_TEST = 'ipc-bench-happy-600'
_BACKTRACE = re.compile(r'\s*_BACKTRACE_TRIPLES\s+"[^"]*"')


def normalise(text: str, source: Path, build: Path) -> str:
    """Replace absolute tree paths so two checkouts compare equal."""
    # Build first: it is normally nested inside the source tree.
    text = text.replace(str(build.resolve()), '<BUILD>')
    return text.replace(str(source.resolve()), '<SOURCE>')


def source_digest(source: Path) -> list[str]:
    root = source / BENCH_DIR
    if not root.is_dir():
        raise FileNotFoundError(f'{root} missing')
    return [f'{path.relative_to(source).as_posix()} '
            f'{hashlib.sha256(path.read_bytes()).hexdigest()}'
            for path in sorted(root.rglob('*')) if path.is_file()]


def registration_lines(build: Path) -> list[str]:
    marker = f'[=[{BENCH_TEST}]=]'
    lines = []
    for testfile in sorted(build.rglob('CTestTestfile.cmake')):
        for line in testfile.read_text(encoding='utf-8').splitlines():
            if marker in line:
                lines.append(_BACKTRACE.sub('', line))
    if not lines:
        raise LookupError(f'{BENCH_TEST} is not registered under {build}')
    return lines


def build_commands(build: Path) -> list[str]:
    result = subprocess.run(['ninja', '-C', str(build), '-t', 'commands', BENCH_TARGET],
                            capture_output=True, text=True, timeout=60, check=False)
    if result.returncode != 0 or not result.stdout.strip():
        raise RuntimeError(f'ninja -t commands failed: {result.stderr.strip()}')
    return result.stdout.splitlines()


def fingerprint(source: Path, build: Path) -> dict[str, list[str]]:
    def norm(lines: list[str]) -> list[str]:
        return [normalise(line, source, build) for line in lines]
    return {
        'sources': source_digest(source),
        'commands': norm(build_commands(build)),
        'registration': norm(registration_lines(build)),
    }


def compare(base: dict[str, list[str]], head: dict[str, list[str]]) -> list[str]:
    return [key for key in base if base[key] != head.get(key)]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--base-src', type=Path, required=True)
    parser.add_argument('--base-build', type=Path, required=True)
    parser.add_argument('--head-src', type=Path, required=True)
    parser.add_argument('--head-build', type=Path, required=True)
    args = parser.parse_args(argv)
    try:
        base = fingerprint(args.base_src, args.base_build)
        head = fingerprint(args.head_src, args.head_build)
    except Exception as error:  # noqa: BLE001 - every failure must run the gate
        print(f'ipc-bench-gate RUN: fingerprint unavailable: {error}')
        return 2
    changed = compare(base, head)
    if changed:
        print(f'ipc-bench-gate RUN: {BENCH_TEST} inputs differ from base: {", ".join(changed)}')
        return 1
    print(f'ipc-bench-gate DEFER: {BENCH_TEST} sources, build commands and registration '
          'are identical to the PR base')
    return 0


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
