# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/media/cross_decode_overlay.py --scratch BUILD [--sysroot ROOT --manifest JSON]
"""Build the real probe through a synthetic overlay; assert ELF/provenance, NEVER run it."""
from __future__ import annotations

import argparse
from dataclasses import replace
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from decode_cross_fixture import make_overlay, make_sysroot
from tools.build.decode_overlay import Overlay
from tools.build.input_provenance import Inputs, check_inputs
from tools.sysroot.models import SysrootError


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--scratch', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--root-project', action='store_true')
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--build', type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    scratch = args.scratch.resolve()
    scratch.mkdir(parents=True, exist_ok=False)
    if (args.sysroot is None) != (args.manifest is None):
        parser.error('--sysroot and --manifest must be paired')
    if args.sysroot is None:
        sysroot, manifest = make_sysroot(scratch)
    else:
        sysroot, manifest = args.sysroot.resolve(strict=True), args.manifest.resolve(strict=True)
    overlay = make_overlay(scratch)
    # Given a fixture digest independently computed before configure consumes it.
    records = ''.join(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.relative_to(overlay).as_posix()}\n'
                      for path in sorted(overlay.rglob('*')) if path.is_file())
    expected = hashlib.sha256(records.encode('ascii')).hexdigest()
    build = args.build.resolve() if args.build is not None else (
        root / 'build' / scratch.name if args.root_project else scratch / 'build')
    assert not build.exists(), 'fresh cross build required'
    if args.root_project:
        assert not overlay.is_relative_to(root) and not overlay.is_relative_to(build), 'overlay must be external'
        if args.archive is None:
            parser.error('--root-project requires --archive')
    environment = {**os.environ, 'GIT_MASTER': '1', 'PKG_CONFIG_PATH': ''}
    configure_source = root if args.root_project else root / 'tests/media/cross'
    configure_extra = [f'-DAA_GOOGLETEST_ARCHIVE={args.archive.resolve(strict=True)}'] if args.root_project else []
    targets = ['aa_host_smoke', 'aa-decode-probe', 'decode-probe-inputs',
               'decode-probe-metrics', 'decode-probe-discovery'] if args.root_project else []
    commands = (
        ['cmake', '-S', str(configure_source), '-B', str(build), '-G', 'Ninja',
         f'-DCMAKE_TOOLCHAIN_FILE={root}/toolchains/jetson-aarch64.cmake',
         f'-DAA_SYSROOT_ROOTFS={sysroot}', f'-DAA_SYSROOT_PAYLOAD={sysroot}',
         f'-DAA_SYSROOT_MANIFEST={manifest}', f'-DAA_DECODE_GST_OVERLAY={overlay}', '-DBUILD_TESTING=ON',
         *configure_extra],
        ['cmake', '--build', str(build), '--parallel', '4', *(['--target', *targets] if targets else [])],
        ['ctest', '--test-dir', str(build), '-R', '^cross_(contamination.*|runtime_regressions)$',
         '--output-on-failure'],
    )
    # When configuring and building actual probe sources with the real cross toolchain.
    for index, command in enumerate(commands):
        log = scratch / f'step-{index}.log'
        print(f'decode-overlay-cross: step={index} log={log}', flush=True)
        with log.open('w') as stream:
            result = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT,
                                    env=environment, check=False, timeout=180)
        if result.returncode != 0:
            print(log.read_text(), file=sys.stderr)
            return 1
    cache = (build / 'CMakeCache.txt').read_text()
    inventory = json.loads(subprocess.check_output(
        ['ctest', '--test-dir', str(build), '--show-only=json-v1'], text=True))
    expected_tests = {f'cross_contamination_{name}' for name in
                      ('aa-decode-probe', 'decode-probe-inputs', 'decode-probe-metrics', 'decode-probe-discovery')}
    if args.root_project:
        expected_tests.update(('cross_contamination', 'cross_runtime_regressions'))
    registered = {test['name'] for test in inventory['tests']
                  if not args.root_project or test['name'].startswith('cross_')}
    assert registered == expected_tests, 'cross coverage mismatch'
    assert 'AA_DECODE_PROBE_CROSS:STRING=AVAILABLE' in cache, 'AA_DECODE_AVAILABLE_REQUIRED'
    assert f'AA_DECODE_GST_OVERLAY_DIGEST:STRING={expected}' in cache, 'fixture digest mismatch'
    overlay_record = Overlay(overlay, build / 'aa-decode-overlay.sha256', expected)
    compiler = Path('/usr/bin/aarch64-linux-gnu-g++')
    inputs = Inputs(sysroot, build, root, compiler, manifest, sysroot, overlay_record)
    binary = build / ('tools/aa-decode-probe/aa-decode-probe' if args.root_project else 'probe/aa-decode-probe')
    header = subprocess.check_output(['readelf', '-h', str(binary)], text=True)
    assert 'AArch64' in header, 'probe is not an AArch64 ELF'
    report = check_inputs(binary, inputs)
    # Then overlay headers/libraries are sanctioned by the binary-bound exact manifest.
    assert not report.contamination, report.contamination
    assert report.overlay == overlay_record
    selected = tuple(item for item in report.selected if item.category == 'digest-recorded-decode-overlay')
    assert any(item.path.endswith('.h') for item in selected), 'overlay headers not consumed'
    assert any(item.path.endswith('.so') for item in selected), 'overlay libraries not consumed'
    for poisoned, expected_field in (
            (replace(inputs, overlay=None), 'provenance.overlay-arguments'),
            (replace(inputs, overlay=replace(overlay_record, digest='0' * 64)), 'AA_DECODE_OVERLAY_MISMATCH')):
        try:
            check_inputs(binary, poisoned)
        except SysrootError as error:
            assert error.field == expected_field, error
        else:
            raise AssertionError('unbound overlay provenance accepted')
    # Given a valid overlay tuple but a different digest carried by the ELF.
    section = scratch / 'wrong-overlay-section'
    section.write_bytes(b'0' * 64 + b'\0')
    wrong_binary = scratch / 'wrong-binding-probe'
    subprocess.run(['aarch64-linux-gnu-objcopy', '--update-section', f'.aa_decode_overlay={section}',
                    str(binary), str(wrong_binary)], check=True)
    # When checking the altered binary, binding must fail before classifying inputs.
    try:
        check_inputs(wrong_binary, inputs)
    except SysrootError as error:
        assert error.field == 'provenance.overlay-binary-binding', error
    else:
        raise AssertionError('wrong ELF overlay binding accepted')
    if args.root_project:
        smoke = check_inputs(build / 'aa_host_smoke', replace(inputs, overlay=None))
        assert not smoke.contamination, smoke.contamination
        assert smoke.overlay is None
        assert not any(Path(item.path).is_relative_to(overlay) for item in smoke.selected)
        assert any(item.path.endswith('/googletest/src/gtest.cc') for item in smoke.selected), 'archive headers omitted'
    receipt = {'state': 'AVAILABLE', 'machine': 'AArch64', 'overlay_digest': expected,
               'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
               'overlay_selected_inputs': len(selected),
               'cross_contamination_tests': sum(name.startswith('cross_contamination') for name in expected_tests),
               'cross_tests': len(expected_tests), 'runtime_regressions': args.root_project,
               'runtime_executed': False, 'acceptance': report.acceptance,
               'root_project': args.root_project, 'overlay_root': str(overlay),
               'source_root': str(root), 'build_root': str(build),
               'binding_negatives': ['missing-overlay', 'wrong-overlay-digest', 'wrong-elf-digest']}
    (scratch / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('decode-overlay-cross: PASS ' + json.dumps(receipt))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
