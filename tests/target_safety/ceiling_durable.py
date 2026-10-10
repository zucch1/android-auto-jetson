# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_durable.py
"""Real local transport capture, rendezvous visibility, and owned cancellation."""
from __future__ import annotations

import json
import os
import selectors
import signal
import socket
import subprocess
import sys
import tempfile
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
from target_safety import runner, stage1
from target_safety.local_capture import Lifecycle, LocalCapture


def records(receipt: Path) -> list:
    return [json.loads(line) for line in receipt.with_suffix('.lifecycle.jsonl').read_text().splitlines()]


def raw_bytes_and_status() -> None:
    # Given
    for code in (0, 7):
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory, ExitStack() as stack:
            receipt = Path(directory) / 'run.json'
            capture = LocalCapture.reserve(stack, receipt)
            child = f"import os,sys; data=sys.stdin.buffer.read(); os.write(1,b'raw\\xff\\x00'+data); os.write(2,b'err\\xfe'); sys.exit({code})"
            # When
            result = runner.run_owned((sys.executable, '-B', '-c', child), capture=capture,
                                      timeout_seconds=3, stdin=b'payload')
            # Then
            assert result.exit_status == code and not result.timed_out
            assert result.stdout == b'raw\xff\x00payload'.decode(errors='replace')
            assert result.stderr == b'err\xfe'.decode(errors='replace')
            assert Path(capture.paths['stdout']).read_bytes() == b'raw\xff\x00payload'
            assert Path(capture.paths['stderr']).read_bytes() == b'err\xfe'
            assert [event['event'] for event in records(receipt)] == ['reserved', 'started', 'terminal']
            assert records(receipt)[-1]['reaped'] and records(receipt)[-1]['exit_status'] == code
            assert all(Path(path).stat().st_mode & 0o777 == 0o600 for path in capture.paths.values())


def visible_while_waiting_and_sigint() -> None:
    # Given: two independent rendezvous: committed-start stderr and child socket.
    for cancelled in (False, True):
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory, socket.socket(socket.AF_UNIX) as server:
            root = Path(directory)
            receipt = root / 'run.json'
            address = str(root / 'ready.sock')
            server.bind(address)
            server.listen(1)
            server.settimeout(5)
            child = f"import os,socket; os.write(1,b'visible\\xff'); os.write(2,b'error\\x00'); s=socket.socket(socket.AF_UNIX); s.connect({address!r}); s.sendall(str(os.getpid()).encode()); s.recv(1)"
            script = f"""
import json,sys
from pathlib import Path
from contextlib import ExitStack
from dataclasses import asdict
sys.path.insert(0,{str(stage1.WORKTREE / 'tools')!r})
from target_safety.local_capture import LocalCapture
from target_safety.runner import run_owned
try:
    with ExitStack() as stack:
        capture=LocalCapture.reserve(stack,Path({str(receipt)!r}))
        result=run_owned((sys.executable,'-B','-c',{child!r}),capture=capture,timeout_seconds=20)
        print(json.dumps(asdict(result)),flush=True)
except KeyboardInterrupt:
    raise SystemExit(130)
"""
            proc = subprocess.Popen((sys.executable, '-B', '-c', script), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                with server.accept()[0] as connection, selectors.DefaultSelector() as selector:
                    pid = int(connection.recv(100))
                    selector.register(proc.stderr, selectors.EVENT_READ)
                    assert selector.select(5), 'committed start not reported'
                    assert json.loads(proc.stderr.readline())['child_pid'] == pid
                    # When: parent is still awaiting the real child's release.
                    assert proc.poll() is None
                    assert Path(receipt.with_suffix('.remote.stdout')).read_bytes() == b'visible\xff'
                    assert Path(receipt.with_suffix('.remote.stderr')).read_bytes() == b'error\x00'
                    events = records(receipt)
                    assert [event['event'] for event in events] == ['reserved', 'started']
                    assert events[0]['invocation_count'] == 1 and events[0]['caller_pid'] == proc.pid
                    assert events[0]['deadline_monotonic'] > events[0]['monotonic']
                    assert events[1]['child_pid'] == events[1]['child_pgid'] == pid
                    if cancelled:
                        os.kill(proc.pid, signal.SIGINT)
                    else:
                        connection.sendall(b'!')
                    out, err = proc.communicate(timeout=12)
                    # Then
                    assert proc.returncode == (130 if cancelled else 0), (out, err)
                    terminal = records(receipt)[-1]
                    assert terminal['reaped']
                    assert terminal['event'] == ('unwound' if cancelled else 'terminal')
                    assert terminal['exit_status'] == (-signal.SIGTERM if cancelled else 0)
                    assert receipt.with_suffix('.remote.stdout').read_bytes() == b'visible\xff'
                    assert receipt.with_suffix('.remote.stderr').read_bytes() == b'error\x00'
                    if not cancelled:
                        assert json.loads(out)['stdout'] == b'visible\xff'.decode(errors='replace')
            finally:
                if proc.poll() is None:
                    proc.kill()
                    proc.communicate(timeout=3)
                if receipt.with_suffix('.lifecycle.jsonl').exists():
                    started = [event for event in records(receipt) if event['event'] == 'started']
                    if started:
                        try: os.killpg(started[0]['child_pgid'], signal.SIGKILL)
                        except ProcessLookupError: pass


def launch_and_started_journal_failure() -> None:
    # Given
    with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory, ExitStack() as stack:
        receipt = Path(directory) / 'absent.json'
        capture = LocalCapture.reserve(stack, receipt)
        # When / Then
        try: runner.run_owned(('/nonexistent/durable-child',), capture=capture, timeout_seconds=1)
        except FileNotFoundError: pass
        else: raise AssertionError('missing executable succeeded')
        assert [event['event'] for event in records(receipt)] == ['reserved', 'launch_failed']
    original = LocalCapture.record
    for injected in (OSError('injected started journal write failure'), SystemExit(17)):
        def failed_start(self: LocalCapture, event: Lifecycle) -> None:
            if event['event'] == 'started': raise injected
            original(self, event)
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory, ExitStack() as stack:
            capture = LocalCapture.reserve(stack, Path(directory) / 'fault.json')
            with patch.object(LocalCapture, 'record', failed_start), patch.object(runner.subprocess, 'Popen', wraps=subprocess.Popen) as launch:
                # When / Then: real Popen succeeds, unwind occurs, ownership already armed.
                try: runner.run_owned((sys.executable, '-B', '-c', 'import signal; signal.pause()'), capture=capture, timeout_seconds=10)
                except (OSError, SystemExit) as error: assert error is injected
                else: raise AssertionError('unwind swallowed')
                launch.assert_called_once()
            events = records(Path(directory) / 'fault.json')
            assert events[-1]['event'] == 'unwound' and events[-1]['reaped']
            assert events[-1]['exit_status'] == -signal.SIGTERM


def stdin_timeout_is_bounded() -> None:
    # Given: transport never reads a payload larger than the stdin pipe.
    with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory, ExitStack() as stack:
        capture = LocalCapture.reserve(stack, Path(directory) / 'timeout.json')
        # When
        result = runner.run_owned((sys.executable, '-B', '-c', "import os,signal; os.write(2,b'partial'); signal.pause()"),
                                  capture=capture, timeout_seconds=0.3, stdin=b'x' * 8_000_000)
        # Then
        assert result.timed_out and result.exit_status < 0 and result.stderr == 'partial'
        assert records(Path(directory) / 'timeout.json')[-1]['reaped']


def fsync_faults_enforce_launch_boundary() -> None:
    # Given: a real fsync boundary fails either before or after real Popen.
    original = os.fsync
    for fail_at in (1, 2):
        calls = []
        def sync(fd: int) -> None:
            calls.append(fd)
            if len(calls) == fail_at: raise OSError('injected journal fsync fault')
            original(fd)
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory, ExitStack() as stack:
            receipt = Path(directory) / 'sync.json'
            capture = LocalCapture.reserve(stack, receipt)
            with patch('target_safety.local_capture.os.fsync', side_effect=sync), patch.object(runner.subprocess, 'Popen', wraps=subprocess.Popen) as launch:
                # When / Then
                try: runner.run_owned((sys.executable, '-B', '-c', 'import signal; signal.pause()'), capture=capture, timeout_seconds=5)
                except OSError as error: assert 'fsync fault' in str(error)
                else: raise AssertionError('fsync failure swallowed')
                assert launch.call_count == fail_at - 1
            if fail_at == 2:
                assert records(receipt)[-1]['reaped'] and records(receipt)[-1]['exit_status'] < 0


def snapshot_preserves_child_write_offset() -> None:
    # Given: a real writer sets its shared offset, then waits for IPC release.
    with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory, ExitStack() as stack:
        capture = LocalCapture.reserve(stack, Path(directory) / 'offset.json')
        with socket.socket(socket.AF_UNIX) as server:
            address = str(Path(directory) / 'offset.sock')
            server.bind(address)
            server.listen(1)
            server.settimeout(5)
            child = f"import os,socket; os.write(1,b'ABCD'); os.lseek(1,1,0); s=socket.socket(socket.AF_UNIX); s.connect({address!r}); s.recv(1); os.write(1,b'X')"
            proc = subprocess.Popen((sys.executable, '-B', '-c', child), stdout=capture.stdout,
                                    stderr=capture.stderr, start_new_session=True)
            try:
                with server.accept()[0] as connection:
                    # When
                    assert capture.text(capture.stdout) == 'ABCD'
                    connection.sendall(b'!')
                # Then: collection did not redirect the child's next write.
                assert proc.wait(timeout=5) == 0
                assert Path(capture.paths['stdout']).read_bytes() == b'AXCD'
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGKILL)
                    proc.wait(timeout=2)


def fixed_route_preserves_interrupted_history() -> None:
    # Given: an empty historical receipt and an explicitly allowlisted new route.
    with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory:
        root = Path(directory)
        history = root / 'task-5-prerequisites-ceiling-policy-resumed.json'
        history.write_bytes(b'')
        destination = root / 'task-5-prerequisites-ceiling-policy-durable-resume.json'
        passed = stage1.Receipt(argv=[], command='local fake', cwd=directory, exit_status=0, stdout='', stderr='')
        with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', '--receipt', str(destination)]), patch.object(stage1, 'attempt_target', return_value=19) as target:
            # When / Then: the fixed route invokes the existing executor exactly once.
            assert stage1.main() == 19
            target.assert_called_once()
            assert target.call_args.args[1] == destination
        for arguments in (['--receipt'], ['--receipt=' + str(destination)], [str(destination)],
                          ['--receipt', str(root / 'other.json')], ['--receipt', str(destination), '--retry']):
            with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', *arguments]), patch.object(stage1, 'run') as qualify, patch.object(stage1, 'run_owned') as transport:
                try: stage1.main()
                except stage1.ReceiptDestinationError:
                    qualify.assert_not_called()
                    transport.assert_not_called()
                else: raise AssertionError('invalid durable route accepted')
        child = (sys.executable, '-B', '-c', "import sys; sys.stdin.buffer.read(); print('misleading exit-zero success')")
        with patch.object(stage1, 'EVIDENCE', root), patch.object(sys, 'argv', ['stage1.py', '--receipt', str(destination)]), patch.object(stage1, 'run', return_value=passed), patch.object(stage1, 'SSH', child):
            # When: real local child only; valid exit alone is not acceptance.
            assert stage1.main() == 1
        # Then
        receipt = json.loads(destination.read_text())
        assert not receipt['accepted'] and receipt['remote']['exit_status'] == 0
        assert receipt['remote']['stdout'] == 'misleading exit-zero success\n'
        assert all(Path(path).is_file() for path in receipt['local_capture'].values())
        assert history.read_bytes() == b'' and not receipt['task5_completed']
        assert records(destination)[-1]['reaped']


def main() -> int:
    for test in (raw_bytes_and_status, visible_while_waiting_and_sigint,
                 launch_and_started_journal_failure, stdin_timeout_is_bounded,
                 fsync_faults_enforce_launch_boundary, snapshot_preserves_child_write_offset,
                 fixed_route_preserves_interrupted_history):
        test()
        print(f'PASS durable {test.__name__}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
