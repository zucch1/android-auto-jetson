# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_capture_paths.py
"""Local capture path integrity fixtures; fake only the QA process boundary."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

import ceiling_capture


def fixture(root: Path) -> tuple[Path, ...]:
    attempt = root / 'task-5-prerequisites-ceiling-policy-durable-resume.json'
    durable = (attempt, *(attempt.with_suffix(suffix) for suffix in
                         ('.remote.stdout', '.remote.stderr', '.lifecycle.jsonl')))
    for index, path in enumerate(durable):
        path.write_bytes(f'fixture-{index}'.encode())
    summary = {'artifacts': [{'path': str(path), 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
                             for path in durable]}
    (root / 'task-5-durable-prerequisite-attempt-summary.json').write_text(json.dumps(summary))
    (root / 'task-5-prerequisites-ceiling-policy-resumed.json').write_bytes(b'')
    (root / 'task-5-prerequisites-ceiling-policy.json').write_bytes(b'policy')
    (root / 'task-5-prerequisites-ceiling-retry.json').write_bytes(b'retry')
    (root / 'other-history.bin').write_bytes(b'other historical bytes')
    return durable


def occupied_history_preserved() -> None:
    # Given: consumed receipt, all three sidecars, empty interrupted receipt and history.
    with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory:
        root = Path(directory)
        durable = fixture(root)
        output = root / 'output'
        output.mkdir()
        before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in root.iterdir() if path.is_file()}
        result = subprocess.CompletedProcess(['fixture-QA'], 0, b'PASS fixture\n', b'')
        with patch.object(ceiling_capture.stage1, 'EVIDENCE', root), \
                patch.object(sys, 'argv', ['ceiling_capture.py', str(output)]), \
                patch.object(ceiling_capture.subprocess, 'run', return_value=result) as qa:
            # When: capture mode qualifies against an occupied historical route.
            code = ceiling_capture.main()
        # Then: all four durable files and other history survive and appear in the snapshot.
        assert code == 0 and qa.call_count == 1
        report = json.loads((output / 'qualification.json').read_text())
        assert report['historical_receipt_hashes'] == before
        assert all(str(path) in report['historical_receipt_hashes'] for path in durable)
        assert report['durable_receipt_and_sidecars_preserved'] is True
        assert report['historical_unchanged_during_QA'] is True
        assert {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in root.iterdir() if path.is_file()} == before
    print('PASS capture occupied receipt/three sidecars and all history preserved')


def sidecar_damage_rejected_before_qa(tampered: bool) -> None:
    for index in (1, 2, 3):
        # Given: one missing or tampered sidecar with the original recorded digest.
        with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory:
            root = Path(directory)
            durable = fixture(root)
            if tampered:
                durable[index].write_bytes(b'tampered')
            else:
                durable[index].unlink()
            output = root / 'output'
            output.mkdir()
            with patch.object(ceiling_capture.stage1, 'EVIDENCE', root), \
                    patch.object(sys, 'argv', ['ceiling_capture.py', str(output)]), \
                    patch.object(ceiling_capture.subprocess, 'run') as qa:
                # When / Then: integrity failure blocks before the QA process starts.
                try:
                    ceiling_capture.main()
                except (AssertionError, FileNotFoundError):
                    assert qa.call_count == 0
                    assert not any(output.iterdir())
                else:
                    raise AssertionError('damaged sidecar accepted')
    print(f'PASS capture sidecar damage rejected before QA; tampered={tampered}')


def history_change_rejected_after_qa() -> None:
    # Given: valid durable history, with QA changing a non-JSON historical file.
    with tempfile.TemporaryDirectory(dir='/tmp/opencode') as directory:
        root = Path(directory)
        fixture(root)
        output = root / 'output'
        output.mkdir()

        def run(argv: tuple[str, ...], *, cwd: Path, capture_output: bool,
                check: bool, timeout: int) -> subprocess.CompletedProcess[bytes]:
            (root / 'other-history.bin').write_bytes(b'changed during QA')
            return subprocess.CompletedProcess(['fixture-QA'], 0, b'', b'')

        with patch.object(ceiling_capture.stage1, 'EVIDENCE', root), \
                patch.object(sys, 'argv', ['ceiling_capture.py', str(output)]), \
                patch.object(ceiling_capture.subprocess, 'run', side_effect=run) as qa:
            # When / Then: capture rejects changed history despite a successful QA exit.
            try:
                ceiling_capture.main()
            except AssertionError:
                assert qa.call_count == 1
                assert not (output / 'qualification.json').exists()
            else:
                raise AssertionError('changed history accepted')
    print('PASS capture non-JSON historical mutation rejected after QA')


if __name__ == '__main__':
    occupied_history_preserved()
    sidecar_damage_rejected_before_qa(False)
    sidecar_damage_rejected_before_qa(True)
    history_change_rejected_after_qa()
