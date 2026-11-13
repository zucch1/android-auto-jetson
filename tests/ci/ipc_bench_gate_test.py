# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression test for the PR classification in tools/ci/ipc_bench_gate.py.

Builds pairs of synthetic configured trees (real build.ninja files, so the real
`ninja -t commands` path is exercised) at different absolute locations and
checks which differences defer the 600 s gate and which force it to run.
"""
import contextlib
import io
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools' / 'ci'))
import ipc_bench_gate  # noqa: E402

FLAGS = '-g -std=c++20 -Wall -Wextra -Wpedantic -Werror'
REGISTRATION = ('add_test([=[ipc-bench-happy-600]=] "{build}/tools/aa-ipc-bench/aa-ipc-bench" '
                '"--profile" "1280x720@30" "--duration" "{duration}")\n'
                'set_tests_properties([=[ipc-bench-happy-600]=] PROPERTIES  LABELS "ipc;long" '
                'TIMEOUT "630" _BACKTRACE_TRIPLES "{source}/tools/aa-ipc-bench/CMakeLists.txt;'
                '{line};add_test;{source}/tools/aa-ipc-bench/CMakeLists.txt;0;")\n')


def make_tree(root: Path, *, flags: str = FLAGS, transport: str = 'int transport;\n',
              duration: str = '600', line: str = '17', register: bool = True,
              target: str = 'aa-ipc-bench') -> tuple[Path, Path]:
    source = root / 'src'
    build = source / 'build' / 'host-dev'
    bench = source / 'tools' / 'aa-ipc-bench'
    bench.mkdir(parents=True)
    (bench / 'transport.cpp').write_text(transport, encoding='utf-8')
    (bench / 'main.cpp').write_text('int main() { return 0; }\n', encoding='utf-8')
    (build / 'tools' / 'aa-ipc-bench').mkdir(parents=True)
    (build / 'build.ninja').write_text(
        'rule cxx\n'
        f'  command = /usr/bin/c++ -I{source}/tools/aa-ipc-bench {flags} -c $in -o $out\n'
        'rule link\n'
        '  command = /usr/bin/c++ -g $in -o $out -lpthread\n'
        f'build main.o: cxx {source}/tools/aa-ipc-bench/main.cpp\n'
        f'build transport.o: cxx {source}/tools/aa-ipc-bench/transport.cpp\n'
        f'build tools/aa-ipc-bench/aa-ipc-bench: link main.o transport.o\n'
        f'build {target}: phony tools/aa-ipc-bench/aa-ipc-bench\n',
        encoding='utf-8')
    if register:
        (build / 'tools' / 'aa-ipc-bench' / 'CTestTestfile.cmake').write_text(
            REGISTRATION.format(build=build, source=source, duration=duration, line=line),
            encoding='utf-8')
    return source, build


def decide(base_kwargs: dict, head_kwargs: dict) -> int:
    with tempfile.TemporaryDirectory() as base_dir, tempfile.TemporaryDirectory() as head_dir:
        base_src, base_build = make_tree(Path(base_dir), **base_kwargs)
        head_src, head_build = make_tree(Path(head_dir), **head_kwargs)
        with contextlib.redirect_stdout(io.StringIO()):
            return ipc_bench_gate.main(['--base-src', str(base_src), '--base-build', str(base_build),
                                        '--head-src', str(head_src), '--head-build', str(head_build)])


CASES = [
    ('identical trees at different paths defer', {}, {}, 0),
    ('backtrace line shift (CMake edit elsewhere) defers', {}, {'line': '19'}, 0),
    ('changed compile flags run', {}, {'flags': FLAGS + ' -O2'}, 1),
    ('changed bench source runs', {}, {'transport': 'int transport = 1;\n'}, 1),
    ('changed registration runs', {}, {'duration': '60'}, 1),
    ('missing registration is an error (runs)', {}, {'register': False}, 2),
    ('missing ninja target is an error (runs)', {}, {'target': 'other'}, 2),
]


def main() -> int:
    failures = 0
    for name, base_kwargs, head_kwargs, expected in CASES:
        actual = decide(base_kwargs, head_kwargs)
        status = 'ok' if actual == expected else 'FAIL'
        failures += actual != expected
        print(f'{status}: {name}: exit {actual} (expected {expected})')
    if failures:
        print(f'ipc-bench-gate classification FAIL: {failures} case(s)')
        return 1
    print(f'ipc-bench-gate classification PASS: {len(CASES)} cases')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
