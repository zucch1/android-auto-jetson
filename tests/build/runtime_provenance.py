# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tests/build/runtime_provenance.py ROOT BUILD SYSROOT
"""Regression QA using a real built fixture, never Jetson acceptance."""
from __future__ import annotations

from dataclasses import replace
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.build.input_provenance import Inputs, check_inputs
from tools.sysroot.models import SysrootError
from fixtures import CONTENT, TAMPERED, build_manifest, write_manifest, write_payload


def main() -> int:
    root, build, sysroot = (Path(value).resolve() for value in sys.argv[1:])
    inputs = Inputs(sysroot, build, root, Path('/usr/bin/aarch64-linux-gnu-g++'), None, None)
    binary = build / 'aa_host_smoke'
    # Given a genuine root-project C++20 + GoogleTest fixture build.
    # When examining its actual compiler dependencies and link map.
    report = check_inputs(binary, inputs)
    # Then all target inputs are in the fixture sysroot, separately from intrinsics.
    assert not report.contamination, report.contamination
    assert report.acceptance == 'fixture-link-sysroot-not-target'
    print('PASS real fixture selected-input provenance (NOT TARGET ACCEPTANCE)')
    with tempfile.TemporaryDirectory(prefix='runtime-provenance-') as temporary:
        scratch = Path(temporary)
        shutil.copyfile(binary, scratch / binary.name)
        obj = scratch / 'smoke.cpp.o'
        shutil.copyfile(build / 'CMakeFiles/aa_host_smoke.dir/tests/host/smoke.cpp.o', obj)
        dep = scratch / 'smoke.cpp.o.headers.d'
        shutil.copyfile(build / 'compile_commands.json', scratch / 'compile_commands.json')
        link_map = scratch / 'aa_host_smoke.map'
        baseline_map = binary.with_name(binary.name + '.map').read_text()
        baseline_map = baseline_map.replace('LOAD CMakeFiles/', f'LOAD {build}/CMakeFiles/')
        baseline_map = baseline_map.replace('LOAD lib/', f'LOAD {build}/lib/')
        baseline_map = baseline_map.replace(
            f'LOAD {build}/CMakeFiles/aa_host_smoke.dir/tests/host/smoke.cpp.o', f'LOAD {obj}')
        # Given retained evidence containing a host-installed ARM64 libc header.
        dep.write_text('object: /usr/aarch64-linux-gnu/include/stdc-predef.h\n')
        link_map.write_text(baseline_map)
        # When classifying inputs (even though their architecture is ARM64).
        leaked = check_inputs(scratch / binary.name, replace(inputs, build=scratch))
        # Then the selected header is rejected.
        assert any('stdc-predef.h' in value for value in leaked.contamination)
        print('PASS host ARM64 header rejected')
        # Given retained evidence containing host-installed libstdc++.
        dep.write_text(f'object: {sysroot}/usr/include/stdio.h\n')
        link_map.write_text(baseline_map + '\nLOAD /usr/aarch64-linux-gnu/lib/libstdc++.so.6\n')
        # When classifying actual link inputs.
        leaked = check_inputs(scratch / binary.name, replace(inputs, build=scratch))
        # Then the target-runtime library outside sysroot is rejected.
        assert any('libstdc++.so.6' in value for value in leaked.contamination)
        print('PASS host ARM64 runtime rejected')
        # Given other valid retained dependencies but none for the linked smoke object.
        saved_dep = dep.read_bytes()
        dep.unlink()
        try:
            # When checking that binary, unrelated dependency files cannot fill the gap.
            try:
                check_inputs(scratch / binary.name, replace(inputs, build=scratch))
            except SysrootError as error:
                assert error.field == 'provenance.missing-dependencies-or-link-map', error
            else:
                raise AssertionError('missing linked-object dependencies accepted')
        finally:
            dep.write_bytes(saved_dep)
        print('PASS missing linked-object dependency evidence rejected')
        # Given loaded project archives without a matching compiled-object record.
        database = scratch / 'compile_commands.json'
        saved_database = database.read_bytes()
        database.write_text('[]\n')
        try:
            # When checking that binary, project archive headers cannot be omitted.
            try:
                check_inputs(scratch / binary.name, replace(inputs, build=scratch))
            except SysrootError as error:
                assert error.field == 'provenance.archive-object-binding', error
            else:
                raise AssertionError('unbound archive object accepted')
        finally:
            database.write_bytes(saved_database)
        print('PASS missing archive object binding rejected')
        commands = json.loads((build / 'compile_commands.json').read_text())
        command = next(item for item in commands if item['file'].endswith('/tests/host/smoke.cpp'))
        flags = shlex.split(command['command'])
        prefix = flags[:flags.index('-o')]
        empty = scratch / 'empty'
        empty.mkdir()
        # Given missing libc headers, with compiler intrinsic headers still available.
        isolated = [flag.replace(str(sysroot), str(empty)) for flag in prefix
                    if not flag.startswith('-Wp,-MD,')]
        # When preprocessing a libc header with the actual build's isolation flags.
        result = subprocess.run([*isolated, '-E', '-x', 'c++', '-'], input='#include <stdio.h>\n',
                                text=True, capture_output=True, check=False, timeout=30)
        # Then GCC must fail instead of using its installed ARM64 libc headers.
        assert result.returncode != 0 and 'stdio.h' in result.stderr, result
        print('PASS missing target libc header cannot fall back')
        # Given an empty target runtime and disabled implicit linker directories.
        # When asking the actual cross linker to select libc/libstdc++.
        result = subprocess.run([str(inputs.compiler), f'--sysroot={empty}', '-nostdlib',
                                 '-Wl,-nostdlib', f'-L{empty}',
                                 str(build / 'CMakeFiles/aa_host_smoke.dir/tests/host/smoke.cpp.o'),
                                 str(empty / 'usr/lib/libstdc++.so'), str(empty / 'usr/lib/libc.so'),
                                 '-o', str(scratch / 'missing-libs')],
                                capture_output=True, text=True, check=False, timeout=30)
        # Then neither target runtime library can fall back to host cross-runtime.
        assert result.returncode != 0 and str(empty / 'usr/lib/libc.so') in result.stderr, result
        assert str(empty / 'usr/lib/libstdc++.so') in result.stderr, result
        print('PASS missing target libc/libstdc++ cannot fall back')
        # Given missing target startup files in an otherwise clean configure environment.
        result = subprocess.run(['cmake', '-S', str(root / 'tests/build/cross'),
                                 '-B', str(scratch / 'missing-runtime'),
                                 f'-DCMAKE_TOOLCHAIN_FILE={root}/toolchains/jetson-aarch64.cmake',
                                 f'-DAA_SYSROOT_ROOTFS={empty}'], capture_output=True, text=True,
                                check=False, timeout=30)
        # Then configure rejects readiness before compiler checks can use host runtime.
        assert result.returncode != 0 and 'AA_RUNTIME_READY' in result.stderr, result
        print('PASS missing runtime configure cannot fall back')
        # Given tampered manifest inputs and a complete fixture link sysroot.
        manifest_path = scratch / 'manifest.json'
        payload = scratch / 'payload'
        write_manifest(manifest_path, build_manifest('v1', '1.0-1', CONTENT))
        write_payload(payload, TAMPERED)
        configure = ['cmake', '-S', str(root / 'tests/build/cross'),
                     '-B', str(scratch / 'hash-gate'), '-G', 'Ninja',
                     f'-DCMAKE_TOOLCHAIN_FILE={root}/toolchains/jetson-aarch64.cmake',
                     f'-DAA_SYSROOT_ROOTFS={sysroot}', f'-DAA_SYSROOT_MANIFEST={manifest_path}',
                     f'-DAA_SYSROOT_PAYLOAD={payload}']
        # When configuring before enable_language.
        result = subprocess.run(configure, capture_output=True, text=True,
                                check=False, timeout=30)
        # Then hash failure occurs before compiler checks.
        assert result.returncode != 0 and 'HASH_MISMATCH' in result.stderr, result
        assert 'compiler identification' not in result.stdout, result
        print('PASS configure hash gate precedes compiler checks')
        # Given a valid configure followed by post-configure payload tampering.
        (payload / 'usr/share/data').write_bytes(CONTENT)
        configured = subprocess.run(configure, capture_output=True, text=True,
                                    check=False, timeout=30)
        assert configured.returncode == 0, configured
        (payload / 'usr/share/data').write_bytes(TAMPERED)
        # When building the cross smoke target.
        result = subprocess.run(['cmake', '--build', str(scratch / 'hash-gate')],
                                capture_output=True, text=True, check=False, timeout=30)
        # Then the retained build-time hash gate blocks compilation.
        assert result.returncode != 0 and 'HASH_MISMATCH' in result.stdout + result.stderr, result
        assert not tuple((scratch / 'hash-gate').rglob('*.o'))
        print('PASS build-time hash gate blocks post-configure tamper')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
