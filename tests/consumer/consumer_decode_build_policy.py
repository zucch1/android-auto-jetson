# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/consumer/consumer_decode_build_policy.py --case sysroot \
#          --root SRC --archive TAR --logs LOGDIR
"""Direct-target provenance tamper regressions for aareceiver-consumer (task 23).

Synthetic compile/build-policy fixture, NOT target qualification and not a
budget claim. Each case configures the ACTUAL root project for jetson-aarch64
against the fixture-only hash-gated sysroot/overlay pair built by
tests/media/decode_cross_fixture.py, changes one fixture input AFTER configure
without reconfiguring, and proves the direct
`cmake --build <build> --target aareceiver-consumer --parallel 4` build re-runs
the provenance gates before compiling: nonzero exit, the relevant hash-validation
diagnostic, and no consumer object compiled. Restoring the exact bytes then
builds successfully; a second post-configure tamper must fail the same direct
target build again even when the consumer archive is otherwise up to date.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'media'))
from decode_cross_fixture import make_overlay, make_sysroot
from tools.sysroot.manifest import parse_manifest
from tools.sysroot.models import RegularFile

DIAGNOSTICS = {'sysroot': 'HASH_MISMATCH', 'overlay': 'AA_DECODE_OVERLAY_MISMATCH'}
CONSUMER_SOURCE = 'src/consumer/gstreamer_adapter/DecodeHelper.cpp'
TAMPER_MARKER = b'\n/* consumer_decode_build_policy post-configure tamper */\n'


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=sorted(DIAGNOSTICS), required=True)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--logs', type=Path, required=True)
    return parser.parse_args()


def step(command: list[str], log: Path, environment: dict[str, str], timeout: int) -> tuple[int, str]:
    print(f'consumer-decode-tamper: log={log}', flush=True)
    with log.open('w') as stream:
        result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                env=environment, check=False, timeout=timeout)
    return result.returncode, log.read_text()


def sysroot_header(sysroot: Path, manifest_path: Path) -> Path:
    """A manifest-recorded header of the fixture payload."""
    manifest = parse_manifest(manifest_path.read_bytes())
    headers = sorted(str(entry.staged_path) for entry in manifest.files
                     if isinstance(entry, RegularFile) and str(entry.staged_path).endswith('.h'))
    assert headers, 'fixture sysroot manifest records no header'
    header = sysroot / headers[0]
    assert header.is_file(), f'manifest-recorded header missing: {header}'
    return header


def overlay_header(overlay: Path) -> Path:
    """A public declaration header copied into the fixture overlay."""
    headers = sorted(path for path in overlay.rglob('*.h') if path.is_file())
    assert headers, 'fixture overlay copied no header'
    return headers[0]


def inventory(build: Path) -> tuple[Path, dict[str, tuple[int, int, str]]]:
    """Consumer object path from the real compile unit, plus its target dir snapshot."""
    units = json.loads((build / 'compile_commands.json').read_text())
    outputs = [Path(unit['directory']) / unit['output'] for unit in units
               if unit['file'].endswith(CONSUMER_SOURCE)]
    assert len(outputs) == 1, f'consumer target compile unit count: {len(outputs)}'
    consumer_object = outputs[0]
    assert 'aareceiver-consumer.dir' in consumer_object.as_posix(), consumer_object
    target_dir = consumer_object.parent
    snapshot = {str(path.relative_to(target_dir)):
                (path.stat().st_size, path.stat().st_mtime_ns,
                 hashlib.sha256(path.read_bytes()).hexdigest())
                for path in sorted(target_dir.rglob('*')) if path.is_file()}
    return consumer_object, snapshot


def run_case(root: Path, archive: Path, run: Path, scratch: Path, case: str) -> int:
    sysroot, manifest = make_sysroot(scratch)
    overlay = make_overlay(scratch)
    assert not overlay.is_relative_to(root) and not overlay.is_relative_to(run), \
        'overlay must be external'
    build = run / 'build'
    assert not build.exists(), 'fresh cross build required'
    environment = {**os.environ, 'GIT_MASTER': '1', 'PKG_CONFIG_PATH': ''}
    configure = ['cmake', '-S', str(root), '-B', str(build), '-G', 'Ninja',
                 f'-DCMAKE_TOOLCHAIN_FILE={root}/toolchains/jetson-aarch64.cmake',
                 f'-DAA_SYSROOT_ROOTFS={sysroot}', f'-DAA_SYSROOT_PAYLOAD={sysroot}',
                 f'-DAA_SYSROOT_MANIFEST={manifest}', f'-DAA_DECODE_GST_OVERLAY={overlay}',
                 '-DBUILD_TESTING=ON', f'-DAA_GOOGLETEST_ARCHIVE={archive}']
    build_target = ['cmake', '--build', str(build), '--target', 'aareceiver-consumer', '--parallel', '4']
    code, output = step(configure, run / 'step-configure.log', environment, 300)
    if code != 0:
        print(output, file=sys.stderr)
        print('consumer-decode-tamper FAIL: cross configure of the root project')
        return 1
    cache = (build / 'CMakeCache.txt').read_text()
    if 'AA_DECODE_PROBE_CROSS:STRING=AVAILABLE' not in cache:
        print('consumer-decode-tamper FAIL: AA_DECODE_PROBE_CROSS not AVAILABLE')
        return 1
    consumer_object, pristine = inventory(build)
    if pristine:
        print(f'consumer-decode-tamper FAIL: consumer objects pre-exist: {sorted(pristine)}')
        return 1
    compiler_id = sorted(build.rglob('CMakeCXXCompilerId*'))
    if not compiler_id:
        print('consumer-decode-tamper FAIL: no compiler-identification objects in the build')
        return 1
    header = sysroot_header(sysroot, manifest) if case == 'sysroot' else overlay_header(overlay)
    original = header.read_bytes()
    print(f'consumer-decode-tamper: case={case} tamper={header} '
          f'consumer_object={consumer_object.relative_to(build)}', flush=True)
    try:
        # (2) change a fixture input AFTER configure WITHOUT reconfiguring, then
        # (3)+(4) the direct target build must fail the hash gate before compiling.
        header.write_bytes(original + TAMPER_MARKER)
        code, output = step(build_target, run / 'step-tamper1.log', environment, 240)
        if code == 0:
            print('consumer-decode-tamper FAIL: direct target build accepted tampered input')
            return 1
        if DIAGNOSTICS[case] not in output:
            print(output, file=sys.stderr)
            print(f'consumer-decode-tamper FAIL: missing {DIAGNOSTICS[case]} diagnostic')
            return 1
        _, state = inventory(build)
        if state:
            print(f'consumer-decode-tamper FAIL: consumer objects compiled despite tamper: {sorted(state)}')
            return 1
        # (5) restore the exact bytes and build the same direct target successfully.
        header.write_bytes(original)
        if header.read_bytes() != original:
            print('consumer-decode-tamper FAIL: exact input bytes not restored')
            return 1
        code, output = step(build_target, run / 'step-restore.log', environment, 240)
        if code != 0:
            print(output, file=sys.stderr)
            print('consumer-decode-tamper FAIL: direct target build after exact restore')
            return 1
        consumer_object, restored = inventory(build)
        if consumer_object.name not in restored:
            print('consumer-decode-tamper FAIL: no consumer object compiled after restore')
            return 1
        # Tamper again and repeat the direct-target build: the gates must run even
        # when the consumer archive is otherwise up to date (no compile pending).
        header.write_bytes(original + TAMPER_MARKER)
        code, output = step(build_target, run / 'step-tamper2.log', environment, 240)
        if code == 0:
            print('consumer-decode-tamper FAIL: up-to-date direct target build accepted tampered input')
            return 1
        if DIAGNOSTICS[case] not in output:
            print(output, file=sys.stderr)
            print(f'consumer-decode-tamper FAIL: up-to-date build missing {DIAGNOSTICS[case]} diagnostic')
            return 1
        _, second = inventory(build)
        if second != restored:
            print('consumer-decode-tamper FAIL: consumer objects recompiled during gated tamper build')
            return 1
    finally:
        header.write_bytes(original)
    receipt = {'state': 'AVAILABLE', 'case': case, 'diagnostic': DIAGNOSTICS[case],
               'tampered_input': str(header), 'consumer_object': str(consumer_object),
               'compiler_id_objects': len(compiler_id), 'root_project': True,
               'runtime_executed': False, 'budgets_asserted': False,
               'acceptance': 'synthetic-compile-policy-fixture'}
    (run / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'consumer-decode-tamper PASS: {case} direct-target provenance gates '
          f'({DIAGNOSTICS[case]}) {json.dumps(receipt)}')
    return 0


def main() -> int:
    args = arguments()
    root = args.root.resolve(strict=True)
    archive = args.archive.resolve(strict=True)
    logs = args.logs.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    # Fixture inputs live outside the source and log roots; only the cross build
    # tree and its step logs are retained here for QA (root_cross_decode_overlay
    # convention).
    run = Path(tempfile.mkdtemp(prefix='run-', dir=logs))
    with tempfile.TemporaryDirectory(prefix='consumer-decode-tamper-') as external:
        return run_case(root, archive, run, Path(external) / 'fixture', args.case)


if __name__ == '__main__':
    raise SystemExit(main())
