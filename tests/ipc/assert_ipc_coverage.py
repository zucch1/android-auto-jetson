# SPDX-License-Identifier: GPL-3.0-or-later
import json
import subprocess
import sys

EXPECTED = frozenset({
    'ipc-IpcFraming.WireRoundTrip',
    'ipc-IpcFraming.RejectHeaderMutations',
    'ipc-IpcFraming.RejectLengthAndFragmentBounds',
    'ipc-IpcFraming.ReassembleMaximumAndRaggedFrames',
    'ipc-IpcFraming.LossRequiresCompleteIdr',
    'ipc-IpcFraming.RejectInconsistentFragmentsAndReplay',
    'ipc-IpcFraming.MalformedFloodKeepsFixedStorage',
    'ipc-IpcFraming.SocketFragmentationAndTruncation',
    'ipc-bench-happy-600', 'ipc-bench-slow-consumer', 'ipc-bench-cli',
})


def main() -> int:
    inventory = subprocess.run(['ctest', '--test-dir', sys.argv[1], '--show-only=json-v1'],
                               capture_output=True, text=True, timeout=30, check=False)
    if inventory.returncode != 0:
        print('ipc-coverage FAIL: inventory unavailable')
        return 1
    registered = {test['name'] for test in json.loads(inventory.stdout)['tests']}
    missing = sorted(EXPECTED - registered)
    if missing:
        print(f'ipc-coverage FAIL: missing {missing}')
        return 1
    print(f'ipc-coverage PASS: {len(EXPECTED)} required tests registered')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
