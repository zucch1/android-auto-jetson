# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_capacity_route.py
"""Fixed capacity route boundary tests; qualification mocked, transport local only."""
from __future__ import annotations

import hashlib
import json
import os
import sys
import tempfile
from pathlib import Path
from typing import Final
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from target_safety import stage1

NAME: Final = 'task-5-prerequisites-ceiling-capacity250k.json'
SUFFIXES: Final = ('.remote.stdout', '.remote.stderr', '.lifecycle.jsonl')


def qualification(code: int) -> stage1.Receipt:
    return stage1.Receipt(argv=[], command='local fixture', cwd='/tmp/opencode',
                          exit_status=code, stdout='', stderr='qualification fixture')


def exact_route_calls_existing_executor_once() -> None:
    # Given
    root = Path(tempfile.mkdtemp(prefix='capacity-route-dispatch-', dir='/tmp/opencode'))
    destination = root / NAME
    with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', '--receipt', str(destination)]), \
            patch.object(stage1, 'attempt_target', return_value=19) as target, \
            patch.object(stage1, 'attempt') as expanded, patch.object(stage1, 'attempt_ceiling') as ceiling:
        # When
        result = stage1.main()
        # Then
        assert result == 19
        target.assert_called_once()
        assert target.call_args.args[1] == destination
        expanded.assert_not_called()
        ceiling.assert_not_called()
        assert not list(root.iterdir())


def malformed_arguments_reject_before_qualification() -> None:
    # Given
    root = Path(tempfile.mkdtemp(prefix='capacity-route-argv-', dir='/tmp/opencode'))
    destination = root / NAME
    for arguments in (['--receipt'], ['--receipt=' + str(destination)], [str(destination)],
                      ['--receipt', str(root / 'other.json')], ['--receipt', str(destination), '--retry'],
                      ['--receipt', str(destination), '--receipt', str(destination)]):
        with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', *arguments]), \
                patch.object(stage1, 'run') as qualify, patch.object(stage1, 'run_owned') as transport:
            # When / Then
            try:
                stage1.main()
            except stage1.ReceiptDestinationError as error:
                assert error.arguments == arguments
            else:
                raise AssertionError('malformed capacity route accepted')
            qualify.assert_not_called()
            transport.assert_not_called()
            assert not list(root.iterdir())


def qualification_failure_reserves_nothing() -> None:
    # Given
    root = Path(tempfile.mkdtemp(prefix='capacity-route-qual-', dir='/tmp/opencode'))
    destination = root / NAME
    with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', '--receipt', str(destination)]), \
            patch.object(stage1, 'run', return_value=qualification(7)) as qualify, \
            patch.object(stage1, 'run_owned') as transport:
        # When
        result = stage1.main()
        # Then
        assert result == 1
        qualify.assert_called_once()
        transport.assert_not_called()
        assert not list(root.iterdir())


def collisions_preserve_history_without_transport() -> None:
    # Given: receipt and each sidecar, regular and dangling collisions.
    for suffix in ('.json', *SUFFIXES):
        for dangling in (False, True):
            root = Path(tempfile.mkdtemp(prefix='capacity-route-collision-', dir='/tmp/opencode'))
            destination = root / NAME
            occupied = destination.with_suffix(suffix)
            history = root / 'historical.json'
            history.write_bytes(b'unchanged history')
            if dangling:
                occupied.symlink_to(root / 'absent')
            else:
                occupied.write_bytes(b'occupied history')
            with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', '--receipt', str(destination)]), \
                    patch.object(stage1, 'run', return_value=qualification(0)), \
                    patch.object(stage1, 'run_owned') as transport:
                # When / Then
                try:
                    stage1.main()
                except FileExistsError:
                    transport.assert_not_called()
                else:
                    raise AssertionError('collision allowed transport')
            assert history.read_bytes() == b'unchanged history'
            assert os.path.lexists(occupied)
            if dangling:
                assert occupied.is_symlink() and os.readlink(occupied) == str(root / 'absent')
                assert not os.path.lexists(root / 'absent')
            else:
                assert occupied.read_bytes() == b'occupied history'
            if suffix != '.json':
                assert destination.read_bytes() == b''


def misleading_exit_zero_is_not_accepted() -> None:
    # Given: only SSH argv is replaced, with a real local child reading the real payload.
    root = Path(tempfile.mkdtemp(prefix='capacity-route-exit0-', dir='/tmp/opencode'))
    destination = root / NAME
    child = (sys.executable, '-B', '-c', "import os,sys; sys.stdin.buffer.read(); os.write(1,b'misleading success\\xff\\x00'); os.write(2,b'local error\\xfe')")
    payload_hash = hashlib.sha256(stage1.supervisor_bundle()).hexdigest()
    with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', '--receipt', str(destination)]), \
            patch.object(stage1, 'run', return_value=qualification(0)), patch.object(stage1, 'SSH', child):
        # When
        result = stage1.main()
    # Then
    receipt = json.loads(destination.read_text())
    assert result == 1 and not receipt['accepted'] and receipt['remote']['exit_status'] == 0
    assert receipt['remote_stdin_sha256'] == payload_hash
    assert destination.with_suffix('.remote.stdout').read_bytes() == b'misleading success\xff\x00'
    assert destination.with_suffix('.remote.stderr').read_bytes() == b'local error\xfe'
    events = [json.loads(line) for line in destination.with_suffix('.lifecycle.jsonl').read_text().splitlines()]
    assert [event['event'] for event in events] == ['reserved', 'started', 'terminal']
    assert events[0]['invocation_count'] == 1 and events[-1]['reaped']
    assert not receipt['task5_completed']


def main() -> int:
    for test in (exact_route_calls_existing_executor_once, malformed_arguments_reject_before_qualification,
                 qualification_failure_reserves_nothing, collisions_preserve_history_without_transport,
                 misleading_exit_zero_is_not_accepted):
        test()
        print(f'PASS capacity route {test.__name__}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
