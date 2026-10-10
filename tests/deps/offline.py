# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/deps/offline.py ROOT ARCHIVE EVIDENCE_JSON
"""Record real task-3 configure/test evidence with namespace or syscall network denial."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import TypedDict


class Command(TypedDict):
    argv: list[str]
    cwd: str
    exit_status: int
    stdout: str
    stderr: str
    stdout_sha256: str
    stderr_sha256: str


def run(argv: list[str], root: Path) -> Command:
    result = subprocess.run(argv, cwd=root, capture_output=True, text=True, check=False)
    return {
        'argv': argv, 'cwd': str(root), 'exit_status': result.returncode,
        'stdout': result.stdout, 'stderr': result.stderr,
        'stdout_sha256': hashlib.sha256(result.stdout.encode()).hexdigest(),
        'stderr_sha256': hashlib.sha256(result.stderr.encode()).hexdigest(),
    }


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    archive = Path(sys.argv[2]).resolve()
    evidence = Path(sys.argv[3]).resolve()
    workspace = Path(tempfile.mkdtemp(prefix='task3-qa-', dir=root / 'build'))
    build = workspace / 'configure'
    reproduced = workspace / 'independent-effective-CMakeLists.txt'
    commands: list[Command] = []
    outcome = 'blocked'
    method = ''
    network_attempts: list[str] = []
    namespace_interfaces_verified = False
    try:
        checker = run([sys.executable, 'tools/deps/check_manifest.py',
                       '--lock', 'deps/manifest.lock'], root)
        commands.append(checker)
        assert checker['exit_status'] == 0, checker
        patched = run(['patch', '--batch', '--fuzz=0', '--posix', '--output', str(reproduced),
                       '--input', str(root / 'deps/patches/aasdk-googletest.patch'),
                       str(root / 'third_party/aasdk/CMakeLists.txt')], root)
        commands.append(patched)
        assert patched['exit_status'] == 0, patched
        assert hashlib.sha256(reproduced.read_bytes()).hexdigest() == 'f7f19407c890e75e68c723347ce63cdc80e3d107d869dd94067c103cf010c818'
        namespace = run(['unshare', '-Urn', 'ip', '-o', 'link', 'show'], root)
        commands.append(namespace)
        configure = ['cmake', '-S', str(root), '-B', str(build), '-G', 'Ninja',
                     f'-DAA_GOOGLETEST_ARCHIVE={archive}']
        if namespace['exit_status'] == 0:
            interfaces = [line.split(':', 2)[1].strip() for line in namespace['stdout'].splitlines()]
            assert interfaces == ['lo'], namespace
            namespace_interfaces_verified = True
            method = 'unshare-user-network-namespace'
            configured = run(['unshare', '-Urn', *configure], root)
        else:
            method = 'strace-ptrace-syscall-denial'
            deny = ['strace', '-f', '-e', 'trace=network']
            for syscall in ('socket', 'socketpair', 'connect', 'sendto', 'sendmsg', 'sendmmsg'):
                deny.extend(['-e', f'inject={syscall}:error=EACCES'])
            self_trace = workspace / 'deny-selftest.trace'
            selftest = run([*deny, '-o', str(self_trace), sys.executable, '-c',
                            "import socket; socket.create_connection(('198.51.100.1', 443), timeout=1)"], root)
            commands.append(selftest)
            trace = self_trace.read_text()
            assert selftest['exit_status'] == 1 and 'AF_INET' in trace and '(INJECTED)' in trace, selftest
            configured = run([*deny, '-o', str(workspace / 'configure-network.trace'), *configure], root)
            trace = (workspace / 'configure-network.trace').read_text()
            network_attempts = [line for line in trace.splitlines()
                                if re.search(r'AF_INET6?\b|connect\(|sendto\(|sendmsg\(|sendmmsg\(', line)]
            assert not network_attempts, network_attempts
        commands.append(configured)
        assert configured['exit_status'] == 0, configured
        fragment = next(build.glob('deps/*/googletest-declaration.cmake'))
        reuse = run([*configure, '--trace-expand', f'--trace-source={fragment}'], root)
        commands.append(reuse)
        assert reuse['exit_status'] == 0, reuse
        assert 'GIT_TAG b514bdc898e2951020cbdca1304b75f5950d1f59' in reuse['stderr'], reuse
        built = run(['cmake', '--build', str(build), '--target', 'gtest', 'gmock', '-j', '2'], root)
        commands.append(built)
        assert built['exit_status'] == 0, built
        tested = run(['ctest', '--test-dir', str(build), '--output-on-failure', '-j', '2'], root)
        commands.append(tested)
        assert tested['exit_status'] == 0, tested
        missing = run(['cmake', '-S', str(root), '-B', str(workspace / 'missing-archive'), '-G', 'Ninja'], root)
        commands.append(missing)
        assert missing['exit_status'] == 1 and 'AA_GOOGLETEST_ARCHIVE' in missing['stderr'], missing
        package_names = [package['name'] for package in json.loads((root / 'deps/manifest.json').read_text())['host_packages']]
        for argv in (
            ['dpkg-query', '-W', '-f=${binary:Package}\t${Version}\t${Architecture}\t${db:Status-Abbrev}\n', *package_names],
            ['apt-cache', 'policy', *package_names], ['protoc', '--version'],
        ):
            commands.append(run(argv, root))
        outcome = 'completed-task3-dependency-only'
    except (AssertionError, OSError) as error:
        print(f'task3 QA failed: {error}', file=sys.stderr)
    finally:
        artifacts = [root / path for path in (
            'deps/manifest.json', 'deps/manifest.lock', 'deps/patches/aasdk-googletest.patch',
            'tools/deps/check_manifest.py', 'tools/deps/stage.py', 'CMakeLists.txt',
            'tests/deps/scenarios.py', 'tests/deps/staging.py', 'tests/deps/offline.py',
            'docs/DEPENDENCIES.md', 'third_party/aasdk/CMakeLists.txt',
        )]
        identities = [json.loads(path.read_text()) for path in build.glob('deps/*/identity.json')]
        receipt = {
            'task': 3, 'outcome': outcome, 'worktree': str(root), 'commands': commands,
            'offline_method': method, 'namespace_interfaces_verified': namespace_interfaces_verified,
            'connection_attempts': network_attempts, 'workspace': str(workspace),
            'stage_identities': identities,
            'artifact_sha256': {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in artifacts if path.is_file()},
            'network_trace_sha256': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                     for path in workspace.glob('*.trace')},
            'independent_effective_cmake_sha256': hashlib.sha256(reproduced.read_bytes()).hexdigest()
            if reproduced.is_file() else None,
            'limitations': [
                'Namespace permission denied requires ptrace syscall-denial fallback; no interface-isolation claim.'
                if not namespace_interfaces_verified else 'Namespace contained only loopback.',
                'Only GoogleTest/GoogleMock dependency targets compiled; full AASDK ABI deferred to task7.',
                'Host development packages missing; candidates are availability observations, not installed pins.',
                'No target contact; task5 immutable sysroot/package/content binding remains pending.',
                'No phone captures or identifiers used; no target or production-media qualification claimed.',
            ],
        }
        evidence.write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'task3={outcome} evidence={evidence} workspace={workspace}')
    return 0 if outcome == 'completed-task3-dependency-only' else 1


if __name__ == '__main__':
    raise SystemExit(main())
