# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Separate local setting executables; strict argv, synthetic authority only."""
from __future__ import annotations

import os
from pathlib import Path

from target_safety.setting import SudoSetting

_SOURCE = """#!/usr/bin/env python3
import json, os, sys, time
from pathlib import Path
root = Path(__file__).parent
args = sys.argv[1:]
with (root / 'argv.jsonl').open('a') as log:
    log.write(json.dumps([sys.argv[0], *args]) + '\\n')
state = root / 'state'
sysctl = str(root / 'fake-sysctl')
assignments = ['fs.inotify.max_user_watches=65536', 'fs.inotify.max_user_watches=1048576']
if Path(sys.argv[0]).name == 'fake-sysctl':
    if args != ['-n', 'fs.inotify.max_user_watches']: sys.exit(92)
    mode = os.environ.get('AA_FAKE_READ', 'ok')
    if mode == 'hang': time.sleep(60)
    if mode == 'nonzero': sys.exit(7)
    print('malformed' if mode == 'malformed' else state.read_text())
    sys.exit(0)
if args == ['-n', '-l', '-l']:
    if os.environ.get('AA_FAKE_LIST_FAIL') == '1': sys.exit(8)
    if (root / 'policy.txt').exists():
        print((root / 'policy.txt').read_text(), end='')
        sys.exit(0)
    option = 'authenticate' if os.environ.get('AA_FAKE_PASSWD') == '1' else '!authenticate'
    print('Sudoers entry:\\n    RunAsUsers: root\\n    Options: ' + option + '\\n    Commands:')
    for assignment in assignments: print('        ' + sysctl + ' -w ' + assignment)
    sys.exit(0)
for assignment in assignments:
    if args == ['-n', '-u', 'root', '-l', '--', sysctl, '-w', assignment]:
        if assignment.endswith('=1048576') and os.environ.get('AA_FAKE_DENY_TEMPORARY') == '1': sys.exit(9)
        sys.exit(0 if os.environ.get('AA_FAKE_WRITE_OK', '1') == '1' else 9)
    if args == ['-n', '-u', 'root', '--', sysctl, '-w', assignment]:
        state.write_text(assignment.split('=')[1])
        sys.exit(0)
sys.exit(93)
"""


def fake_setting(root: Path, write_ok: bool = True) -> tuple[SudoSetting, Path]:
    """Create strict nonprivileged read and privileged-write authority fixtures."""
    for name in ("fake-sudo", "fake-sysctl"):
        exe = root / name
        exe.write_text(_SOURCE)
        exe.chmod(0o755)
    state = root / "state"
    state.write_text("65536")
    os.environ["AA_FAKE_WRITE_OK"] = "1" if write_ok else "0"
    return SudoSetting(sudo=(str(root / "fake-sudo"), "-n"),
                       sysctl=str(root / "fake-sysctl")), state
