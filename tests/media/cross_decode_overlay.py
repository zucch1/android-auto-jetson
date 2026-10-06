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
    build = scratch / 'build'
    environment = {**os.environ, 'GIT_MASTER': '1', 'PKG_CONFIG_PATH': ''}
    commands = (
        ['cmake', '-S', str(root / 'tests/media/cross'), '-B', str(build), '-G', 'Ninja',
         f'-DCMAKE_TOOLCHAIN_FILE={root}/toolchains/jetson-aarch64.cmake',
         f'-DAA_SYSROOT_ROOTFS={sysroot}', f'-DAA_SYSROOT_PAYLOAD={sysroot}',
         f'-DAA_SYSROOT_MANIFEST={manifest}', f'-DAA_DECODE_GST_OVERLAY={overlay}', '-DBUILD_TESTING=ON'],
        ['cmake', '--build', str(build), '--parallel', '4'],
        ['ctest', '--test-dir', str(build), '-R', '^cross_contamination_', '--output-on-failure'],
    )
    # When configuring and building actual probe sources with the real cross toolchain.
    for index, command in enumerate(commands):
        log = scratch / f'step-{index}.log'
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
    assert {test['name'] for test in inventory['tests']} == expected_tests, 'cross coverage mismatch'
    assert 'AA_DECODE_PROBE_CROSS:STRING=AVAILABLE' in cache, 'AA_DECODE_AVAILABLE_REQUIRED'
    assert f'AA_DECODE_GST_OVERLAY_DIGEST:STRING={expected}' in cache, 'fixture digest mismatch'
    overlay_record = Overlay(overlay, build / 'aa-decode-overlay.sha256', expected)
    compiler = Path('/usr/bin/aarch64-linux-gnu-g++')
    inputs = Inputs(sysroot, build, root, compiler, manifest, sysroot, overlay_record)
    binary = build / 'probe/aa-decode-probe'
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
    receipt = {'state': 'AVAILABLE', 'machine': 'AArch64', 'overlay_digest': expected,
               'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
               'overlay_selected_inputs': len(selected), 'cross_contamination_tests': len(expected_tests),
               'runtime_executed': False, 'acceptance': report.acceptance}
    (scratch / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('decode-overlay-cross: PASS ' + json.dumps(receipt))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
