# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tests/build/scenarios.py ROOT CASE
"""Fixture-only task-5 build acceptance checks; every case asserts it is NOT target acceptance."""
from __future__ import annotations

import json
from pathlib import Path
import sys
import tempfile

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from fixtures import (CONTENT, TAMPERED, build_manifest, compile_binary, frozen_artifact,
                      run_tool, write_manifest, write_payload)
from tools.sysroot.models import Architecture

SMOKE = Path(__file__).resolve().parent / 'cross' / 'smoke.cpp'


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    case = sys.argv[2]
    print(f'FIXTURE-ONLY NOT-TARGET-ACCEPTANCE case={case}')
    with tempfile.TemporaryDirectory(prefix=f'build-{case}-') as temporary:
        fixture = Path(temporary)
        baseline = build_manifest('v1', '1.0-1', CONTENT)
        baseline_path = fixture / 'baseline.json'
        write_manifest(baseline_path, baseline)
        match case:
            case 'validate-happy':
                payload = fixture / 'payload'
                write_payload(payload, CONTENT)
                result = run_tool(root, 'validate_sysroot.py', '--manifest', str(baseline_path),
                                  '--payload', str(payload))
                assert result.returncode == 0, result
                assert 'fixture-not-target' in result.stdout, result
            case 'validate-tamper':
                payload = fixture / 'payload'
                write_payload(payload, TAMPERED)
                result = run_tool(root, 'validate_sysroot.py', '--manifest', str(baseline_path),
                                  '--payload', str(payload))
                assert result.returncode == 1, result
                assert 'HASH_MISMATCH' in result.stdout + result.stderr, result
            case 'drift-none':
                result = run_tool(root, 'drift.py', '--baseline', str(baseline_path),
                                  '--candidate', str(baseline_path))
                report = json.loads(result.stdout)
                assert result.returncode == 0 and not report['new_version_required'], report
            case 'drift-package-version':
                candidate = fixture / 'candidate.json'
                write_manifest(candidate, build_manifest('v2', '2.0-1', CONTENT))
                result = run_tool(root, 'drift.py', '--baseline', str(baseline_path),
                                  '--candidate', str(candidate))
                report = json.loads(result.stdout)
                assert result.returncode == 1 and report['new_version_required'], report
                assert report['package_version_drift'] and 'fixture-data' in report['changed_packages'], report
            case 'drift-same-version-content':
                candidate = fixture / 'candidate.json'
                write_manifest(candidate, build_manifest('v1', '1.0-1', TAMPERED))
                result = run_tool(root, 'drift.py', '--baseline', str(baseline_path),
                                  '--candidate', str(candidate))
                report = json.loads(result.stdout)
                assert result.returncode == 1 and report['new_version_required'], report
                assert report['content_drift'] and report['same_version_content_drift'], report
            case 'drift-package-identity':
                candidate = fixture / 'candidate.json'
                write_manifest(candidate, build_manifest('v1', '1.0-1', CONTENT,
                                                         source='synthetic-alternate'))
                result = run_tool(root, 'drift.py', '--baseline', str(baseline_path),
                                  '--candidate', str(candidate))
                report = json.loads(result.stdout)
                assert result.returncode == 1 and report['new_version_required'], report
                assert report['package_version_drift'], report
                assert 'fixture-data' in report['changed_packages'], report
            case 'drift-package-architecture':
                candidate = fixture / 'candidate.json'
                write_manifest(candidate, build_manifest('v1', '1.0-1', CONTENT,
                                                         architecture=Architecture.ALL,
                                                         all_justification='arch-independent fixture data'))
                result = run_tool(root, 'drift.py', '--baseline', str(baseline_path),
                                  '--candidate', str(candidate))
                report = json.loads(result.stdout)
                assert result.returncode == 1 and report['new_version_required'], report
                assert report['package_version_drift'], report
                assert 'fixture-data' in report['changed_packages'], report
            case 'cache-bind':
                cache = fixture / 'cache'
                result = run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                                  '--manifest', str(baseline_path))
                assert result.returncode == 0, result
                assert (cache / 'current.json').exists(), 'cache current pointer missing'
            case 'cache-reuse':
                cache = fixture / 'cache'
                run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                         '--manifest', str(baseline_path))
                result = run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                                  '--manifest', str(baseline_path))
                assert result.returncode == 0 and json.loads(result.stdout)['namespace_reused'], result
            case 'cache-drift-same-version':
                cache = fixture / 'cache'
                run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                         '--manifest', str(baseline_path))
                candidate = fixture / 'candidate.json'
                write_manifest(candidate, build_manifest('v1', '1.0-1', TAMPERED))
                result = run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                                  '--manifest', str(candidate))
                assert result.returncode == 1, result
                assert 'NEW_VERSION_REQUIRED' in result.stdout + result.stderr, result
            case 'cache-publish':
                artifact = frozen_artifact(fixture / 'src', 'v1', CONTENT)
                cache = fixture / 'cache'
                result = run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact),
                                  '--cache-root', str(cache))
                assert result.returncode == 0, result
                report = json.loads(result.stdout)
                assert report['acceptance'] == 'fixture-not-target', report
                assert (cache / 'objects' / f"{report['archive_sha256']}.tar").exists(), 'archive bytes missing'
                assert (cache / 'bindings' / f"{report['manifest_digest']}.json").exists(), 'manifest binding missing'
                check = run_tool(root, 'cache_publish.py', 'verify', '--cache-root', str(cache),
                                 '--manifest-sha256', report['manifest_digest'])
                assert check.returncode == 0, check
            case 'cache-publish-idempotent':
                artifact = frozen_artifact(fixture / 'src', 'v1', CONTENT)
                cache = fixture / 'cache'
                run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact),
                         '--cache-root', str(cache))
                result = run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact),
                                  '--cache-root', str(cache))
                assert result.returncode == 0 and json.loads(result.stdout)['archive_reused'], result
            case 'cache-publish-deterministic':
                artifact = frozen_artifact(fixture / 'src', 'v1', CONTENT)
                first = json.loads(run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact),
                                            '--cache-root', str(fixture / 'cache-a')).stdout)
                second = json.loads(run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact),
                                             '--cache-root', str(fixture / 'cache-b')).stdout)
                assert first['archive_sha256'] == second['archive_sha256'], (first, second)
            case 'cache-publish-tamper':
                artifact = frozen_artifact(fixture / 'src', 'v1', CONTENT)
                cache = fixture / 'cache'
                report = json.loads(run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact),
                                             '--cache-root', str(cache)).stdout)
                archive = cache / 'objects' / f"{report['archive_sha256']}.tar"
                archive.chmod(0o644)  # simulate an attacker able to chmod; the cache is otherwise frozen
                corrupted = bytearray(archive.read_bytes())
                corrupted[0] ^= 0xFF
                archive.write_bytes(bytes(corrupted))
                result = run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact),
                                  '--cache-root', str(cache))
                assert result.returncode == 1, result
                assert 'HASH_MISMATCH' in result.stdout + result.stderr, result
            case 'cache-publish-drift':
                artifact_a = frozen_artifact(fixture / 'src-a', 'v1', CONTENT)
                artifact_b = frozen_artifact(fixture / 'src-b', 'v1', TAMPERED)
                cache = fixture / 'cache'
                run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact_a),
                         '--cache-root', str(cache))
                result = run_tool(root, 'cache_publish.py', 'publish', '--artifact', str(artifact_b),
                                  '--cache-root', str(cache))
                assert result.returncode == 1, result
                assert 'NEW_VERSION_REQUIRED' in result.stdout + result.stderr, result
            case 'cache-publish-verify-binding-mismatch':
                artifact = frozen_artifact(fixture / 'src', 'v1', CONTENT)
                cache = fixture / 'cache'
                report = json.loads(run_tool(root, 'cache_publish.py', 'publish', '--artifact',
                                             str(artifact), '--cache-root', str(cache)).stdout)
                binding = cache / 'bindings' / f"{report['manifest_digest']}.json"
                binding.chmod(0o644)
                record = json.loads(binding.read_text())
                record['manifest_digest'] = '0' * 64
                binding.write_text(json.dumps(record, sort_keys=True))
                result = run_tool(root, 'cache_publish.py', 'verify', '--cache-root', str(cache),
                                  '--manifest-sha256', report['manifest_digest'])
                assert result.returncode == 1, result
                assert 'ARTIFACT_MISMATCH' in result.stdout + result.stderr, result
            case 'cache-publish-verify-provenance-tamper':
                artifact = frozen_artifact(fixture / 'src', 'v1', CONTENT)
                cache = fixture / 'cache'
                report = json.loads(run_tool(root, 'cache_publish.py', 'publish', '--artifact',
                                             str(artifact), '--cache-root', str(cache)).stdout)
                binding = cache / 'bindings' / f"{report['manifest_digest']}.json"
                binding.chmod(0o644)
                record = json.loads(binding.read_text())
                record['provenance'] = 'observed'
                record['acceptance'] = 'target-observed'
                binding.write_text(json.dumps(record, sort_keys=True))
                result = run_tool(root, 'cache_publish.py', 'verify', '--cache-root', str(cache),
                                  '--manifest-sha256', report['manifest_digest'])
                assert result.returncode == 1, result
                assert 'ARTIFACT_MISMATCH' in result.stdout + result.stderr, result
                observed = run_tool(root, 'cache_publish.py', 'verify', '--cache-root', str(cache),
                                    '--manifest-sha256', report['manifest_digest'],
                                    '--require-observed')
                assert observed.returncode == 1, observed
                assert 'FIXTURE_PROVENANCE' in observed.stdout + observed.stderr, observed
            case 'cache-publish-verify-version-tamper':
                artifact = frozen_artifact(fixture / 'src', 'v1', CONTENT)
                cache = fixture / 'cache'
                report = json.loads(run_tool(root, 'cache_publish.py', 'publish', '--artifact',
                                             str(artifact), '--cache-root', str(cache)).stdout)
                binding = cache / 'bindings' / f"{report['manifest_digest']}.json"
                binding.chmod(0o644)
                record = json.loads(binding.read_text())
                record['version'] = 'tampered-version'
                binding.write_text(json.dumps(record, sort_keys=True))
                result = run_tool(root, 'cache_publish.py', 'verify', '--cache-root', str(cache),
                                  '--manifest-sha256', report['manifest_digest'])
                assert result.returncode == 1, result
                assert 'ARTIFACT_MISMATCH' in result.stdout + result.stderr, result
            case 'cache-bind-current-field-mismatch':
                cache = fixture / 'cache'
                run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                         '--manifest', str(baseline_path))
                current = cache / 'current.json'
                record = json.loads(current.read_text())
                record['version'] = 'tampered-version'
                current.write_text(json.dumps(record, sort_keys=True))
                result = run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                                  '--manifest', str(baseline_path))
                assert result.returncode == 1, result
                assert 'ARTIFACT_MISMATCH' in result.stdout + result.stderr, result
            case 'cache-bind-rebound-prior-mismatch':
                cache = fixture / 'cache'
                run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                         '--manifest', str(baseline_path))
                namespace = cache / json.loads((cache / 'current.json').read_text())['manifest_digest']
                prior_path = namespace / 'binding.json'
                prior_path.chmod(0o644)
                prior = json.loads(prior_path.read_text())
                prior['version'] = 'tampered-prior'
                prior_path.write_text(json.dumps(prior, sort_keys=True))
                candidate = fixture / 'candidate.json'
                write_manifest(candidate, build_manifest('v2', '2.0-1', CONTENT))
                result = run_tool(root, 'cache_binding.py', '--cache-root', str(cache),
                                  '--manifest', str(candidate))
                assert result.returncode == 1, result
                assert 'ARTIFACT_MISMATCH' in result.stdout + result.stderr, result
            case 'contamination-clean':
                binary = fixture / 'aa_clean'
                assert compile_binary('aarch64-linux-gnu-g++', binary, SMOKE).returncode == 0
                result = run_tool(root, 'contamination.py', '--binary', str(binary))
                assert result.returncode == 0 and json.loads(result.stdout)['clean'], result
            case 'contamination-host':
                binary = fixture / 'aa_host'
                assert compile_binary('g++', binary, SMOKE).returncode == 0
                result = run_tool(root, 'contamination.py', '--binary', str(binary))
                report = json.loads(result.stdout)
                assert result.returncode == 1 and not report['clean'], report
                assert report['contamination'], 'host contamination not detected'
            case _:
                raise AssertionError(f'Unknown scenario: {case}')
    print(f'PASS fixture-only NOT-TARGET-ACCEPTANCE case={case}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
