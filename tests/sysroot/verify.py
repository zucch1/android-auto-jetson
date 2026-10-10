# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tests/sysroot/verify.py
"""Run local checks and retain raw command output under this owned test directory."""
from __future__ import annotations

import ast
import hashlib
import json
from pathlib import Path
import subprocess
import sys


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    commands = (
        [sys.executable, '-B', 'tests/sysroot/model.py'],
        [sys.executable, '-B', 'tests/sysroot/staging.py'],
        [sys.executable, '-B', 'tests/sysroot/publication.py'],
        [sys.executable, '-B', 'tools/sysroot/cli.py', '--help'],
        [sys.executable, '-B', 'tools/sysroot/cli.py', 'materialize', '--help'],
        ['bash', '-n', 'tools/sysroot/extract-jetson-sysroot.sh'],
        ['bash', 'tools/sysroot/extract-jetson-sysroot.sh', '--help'],
        [sys.executable, '-B', 'tools/deps/check_manifest.py', '--lock', 'deps/manifest.lock'],
    )
    results = []
    for command in commands:
        result = subprocess.run(command, cwd=root, capture_output=True, text=True, check=False)
        results.append({'command': command, 'exit': result.returncode,
                        'stdout': result.stdout, 'stderr': result.stderr})
        print(f'exit={result.returncode} command={command}')
        print(result.stdout + result.stderr, end='')
    counts = {}
    for folder in ('tools/sysroot', 'tests/sysroot'):
        for path in sorted((root / folder).glob('*.py')):
            source = path.read_text()
            ast.parse(source, filename=str(path))
            compile(source, str(path), 'exec')
            count = sum(bool(line.strip()) and not line.lstrip().startswith('#')
                        for line in source.splitlines())
            counts[str(path.relative_to(root))] = count
            assert count <= 250, (path, count)
    preserved = {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                 for folder in ('tools/target_safety', 'tests/target_safety')
                 for p in sorted((root / folder).glob('*.py'))}
    for name, expected in (
        ('deps/manifest.json', '52b1bc3fce2159d1de1cff8a10e535eb0a4a9f3a0779d531e853b89a177cb33e'),
        ('deps/manifest.lock', '89721697bf8024e8099f6ce7c08fe8a3e1126d4d41dec1d189be381667479dda')):
        actual = hashlib.sha256((root / name).read_bytes()).hexdigest()
        assert actual == expected, name
        preserved[name] = actual
    assert not (root / 'toolchains/jetson-sysroot-manifest.json').exists()
    capture = {'results': results, 'pure_loc': counts, 'ast_syntax': 'PASS',
               'preserved_sha256': preserved,
               'lsp': 'UNAVAILABLE: basedpyright not installed; installation declined',
               'task5': False, 'task6': 'gated'}
    output = root / 'tests/sysroot/local-capture.json'
    output.write_text(json.dumps(capture, indent=2, sort_keys=True) + '\n')
    print(json.dumps({'pure_loc': counts, 'capture': str(output), 'ast_syntax': 'PASS'}))
    return int(any(result['exit'] != 0 for result in results))


if __name__ == '__main__':
    raise SystemExit(main())
