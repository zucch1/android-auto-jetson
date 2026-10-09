# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/deps/staging.py ROOT CASE ARCHIVE
"""Drive staging and the effective dependency CMake block through their real CLIs."""
from pathlib import Path
from enum import StrEnum
import hashlib
import re
import subprocess
import sys
import tempfile
from typing import assert_never


class Case(StrEnum):
    REUSE = 'reuse'
    EFFECTIVE_TAMPER = 'effective-tamper'
    GTEST_TAMPER = 'gtest-tamper'
    EXTRA = 'extra'
    MISSING_STAGING = 'missing-staging'
    TLS = 'tls'
    TLS_TAMPER = 'tls-tamper'


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    case = Case(sys.argv[2])
    archive = Path(sys.argv[3]).resolve()
    original = root / 'third_party/aasdk/src/Messenger/Cryptor.cpp'
    original_digest = hashlib.sha256(original.read_bytes()).hexdigest()
    # Given: a unique cache beneath ignored build/, retained for diagnosis.
    cache = Path(tempfile.mkdtemp(prefix='stage-test-', dir=root / 'build'))
    command = [sys.executable, '-B', str(root / 'tools/deps/stage.py'),
               '--root', str(root), '--cache', str(cache), '--archive', str(archive)]
    initial = subprocess.run(command, capture_output=True, text=True, check=False)
    assert initial.returncode == 0, initial
    stage = Path(initial.stdout.strip())
    expected = ''
    match case:
        case Case.REUSE:
            pass
        case Case.TLS:
            effective = stage / 'aasdk'
            marker = re.compile(rb'-----BEGIN (?:[A-Z0-9]+ )*PRIVATE KEY-----')
            for path in effective.rglob('*'):
                if path.is_file():
                    assert not marker.search(path.read_bytes()), path
            cryptor = (effective / 'src/Messenger/Cryptor.cpp').read_text()
            assert 'cCertificate' not in cryptor and 'cPrivateKey' not in cryptor
            assert 'aa::tls::load_credentials()' in cryptor
            assert 'policy_.require_approved()' in cryptor
            assert not (effective / 'cert/headunit.key').exists()
            assert '/etc/aasdk' not in (effective / 'CMakeLists.txt').read_text()
            assert 'headunit.key' not in (effective / 'debian/postinst').read_text()
            assert hashlib.sha256(original.read_bytes()).hexdigest() == original_digest
        case Case.TLS_TAMPER:
            with (stage / 'aasdk/src/Messenger/Cryptor.cpp').open('ab') as stream:
                stream.write(b'\n// tamper\n')
            expected = 'STAGE_MISMATCH'
        case Case.EFFECTIVE_TAMPER:
            with (stage / 'aasdk/CMakeLists.txt').open('ab') as stream:
                stream.write(b'\n# tamper\n')
            expected = 'STAGE_MISMATCH'
        case Case.GTEST_TAMPER:
            with (stage / 'googletest/googletest/src/gtest.cc').open('ab') as stream:
                stream.write(b'\n// tamper\n')
            expected = 'STAGE_MISMATCH'
        case Case.EXTRA:
            (stage / 'googletest/injected.txt').write_text('tamper\n')
            expected = 'STAGE_MISMATCH'
        case Case.MISSING_STAGING:
            probe = cache / 'probe'
            probe.mkdir()
            # Execute the block copied from effective AASDK, with staging absent.
            (probe / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(missing_stage LANGUAGES NONE)\n'
                'set(AASDK_TEST ON)\n'
                f'include("{stage}/googletest-declaration.cmake")\n')
            command = ['cmake', '-S', str(probe), '-B', str(probe / 'build')]
            expected = 'GoogleTest must be pre-staged and hash-verified'
        case unreachable:
            assert_never(unreachable)
    # When: reuse the cache, or configure without the required staged path.
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    # Then: immutable reuse succeeds; corrupt cache or missing staging fails closed.
    print(f'case={case} exit={result.returncode} cache={cache}')
    print(result.stdout + result.stderr, end='')
    assert result.returncode == (1 if expected else 0), result
    assert not expected or expected in result.stdout + result.stderr, result
    if not expected:
        assert result.stdout == initial.stdout, result
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
