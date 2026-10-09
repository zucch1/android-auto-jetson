# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/media/assert_decode_coverage.py BUILD_DIR
"""Fail CI unless the expected Task 9 decode tests are registered in CTest."""
from pathlib import Path
import json
import subprocess
import sys
from typing import Final

EXPECTED: Final = frozenset({
    'media_decode_probe_suite',
    'decode-probe-inputs',
    'decode-probe-metrics',
    'decode-probe-discovery',
    'decode-build-policy-isolated',
    'decode-build-policy-host-path',
    'decode-build-policy-missing',
    'decode-build-policy-escape',
    'decode-build-policy-symlink',
    'decode-build-policy-unbound',
    'decode-build-policy-ordering',
    'decode-build-policy-partial',
    'decode-build-policy-overlay',
    'decode-build-policy-overlay-escape',
    'decode-build-policy-overlay-poison',
    'decode-build-policy-overlay-partial',
    'decode-build-policy-overlay-comma',
    'decode-build-policy-overlay-semicolon',
    'decode-build-policy-overlay-mismatch',
    'decode-build-policy-overlay-extra',
    'decode-build-policy-overlay-available',
    'decode-cross-root-overlay',
    'decode-probe-smoke',
    'decode-probe-corrections',
})


def main() -> int:
    build_dir = Path(sys.argv[1]).resolve()
    inventory = subprocess.run(
        ['ctest', '--test-dir', str(build_dir), '--show-only=json-v1'],
        capture_output=True, text=True, check=False)
    if inventory.returncode != 0:
        print('decode-coverage: FAIL ctest-inventory-unavailable')
        return 1
    registered = {test['name'] for test in json.loads(inventory.stdout)['tests']}
    missing = sorted(EXPECTED - registered)
    if missing:
        print(f'decode-coverage: FAIL missing-registered-tests {missing}')
        return 1
    print(f'decode-coverage: PASS {len(EXPECTED)} expected tests registered')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
