# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Imported by python3 -B tests/sysroot/staging.py
"""Synthetic ordinary bytes, never development headers or libraries."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import subprocess
import sys
from typing import TypeAlias

Json: TypeAlias = str | int | None | list['Json'] | dict[str, 'Json']


@dataclass(frozen=True, slots=True)
class Fixture:
    root: Path
    snapshot: Path
    metadata: Path
    destination: Path

    def document(self, version: str = 'v1', content: bytes = b'synthetic bytes\n') -> dict[str, Json]:
        return {
            'schema': 'aa-sysroot-1', 'version': version,
            'provenance': 'fixture', 'identity': {'kernel': 'synthetic', 'l4t': None},
            'capability': 'pending',
            'packages': [{'name': 'fixture-data', 'version': '1.0-1',
                          'architecture': 'arm64', 'source': 'synthetic-local',
                          'all_justification': None}],
            'files': [{'kind': 'regular', 'original_input_path': '/usr/share/data',
                       'staged_path': 'usr/share/data', 'package': 'fixture-data',
                       'sha256': hashlib.sha256(content).hexdigest()},
                      {'kind': 'symlink', 'original_input_path': '/usr/share/alias',
                       'staged_path': 'usr/share/alias', 'package': 'fixture-data',
                       'sha256': hashlib.sha256(b'data').hexdigest(),
                       'link_text': 'data'}],
        }

    def write(self, document: dict[str, Json]) -> None:
        self.metadata.write_text(json.dumps(document), encoding='utf-8')

    def run(self, command: str = 'materialize') -> subprocess.CompletedProcess[str]:
        args = [sys.executable, '-B', str(self.root / 'tools/sysroot/cli.py'), command]
        if command == 'wrapper':
            args = ['bash', str(self.root / 'tools/sysroot/extract-jetson-sysroot.sh')]
        if command in ('materialize', 'wrapper'):
            args += ['--snapshot', str(self.snapshot), '--metadata', str(self.metadata),
                     '--destination', str(self.destination)]
        else:
            args += ['--artifact', str(self.destination / 'v1')]
        result = subprocess.run(args, capture_output=True, text=True, check=False)
        print(f'command={command} exit={result.returncode}')
        print(result.stdout + result.stderr, end='')
        return result


def create(root: Path, base: Path) -> Fixture:
    fixture = Fixture(root, base / 'snapshot', base / 'metadata.json', base / 'output')
    (fixture.snapshot / 'usr/share').mkdir(parents=True)
    (fixture.snapshot / 'usr/share/data').write_bytes(b'synthetic bytes\n')
    (fixture.snapshot / 'usr/share/alias').symlink_to('data')
    fixture.destination.mkdir()
    fixture.write(fixture.document())
    return fixture


def expect(result: subprocess.CompletedProcess[str], code: str) -> None:
    assert result.returncode == 1, result
    assert json.loads(result.stderr)['code'] == code, result
