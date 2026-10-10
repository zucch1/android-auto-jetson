# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/tls/synthetic_credential.py ROOT CASE
"""Assert the generator never writes into the source tree and still serves external OUTDIRs.

Cases prove the never-into-the-source-tree contract of
tools/tls/synthetic_credential.py: the repository root, a source subdirectory
and an external symlink that resolves into the source tree must all be
rejected before anything is created or deleted at the destination, while a
plain external temporary directory must still generate the labeled synthetic
credential.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

CERT_NAME = 'synthetic-hu.crt'
KEY_NAME = 'synthetic-hu.key'


def run(tool: Path, destination: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run([sys.executable, '-B', str(tool), str(destination)],
                          capture_output=True, text=True, check=False)


def listing(directory: Path) -> tuple[str, ...]:
    return tuple(sorted(child.name for child in directory.iterdir()))


def assert_rejected(tool: Path, destination: Path) -> None:
    result = run(tool, destination)
    assert result.returncode != 0, result
    assert result.stdout == '', result
    assert 'synthetic-credential-source-tree-destination' in result.stderr, result


def repository_root(root: Path) -> None:
    tool = root / 'tools/tls/synthetic_credential.py'
    fixture_prefix = 'tls-generation-'
    before = tuple(name for name in listing(root) if not name.startswith(fixture_prefix))
    assert_rejected(tool, root)
    after = tuple(name for name in listing(root) if not name.startswith(fixture_prefix))
    assert after == before, (before, after)
    assert not (root / CERT_NAME).exists() and not (root / KEY_NAME).exists()


def source_subdirectory(root: Path) -> None:
    tool = root / 'tools/tls/synthetic_credential.py'
    with tempfile.TemporaryDirectory(prefix='tls-generation-', dir=root) as directory:
        destination = Path(directory)
        sentinels = {}
        for name in (CERT_NAME, KEY_NAME):
            marker = destination / name
            marker.write_bytes(b'sentinel ' + name.encode())
            sentinels[marker] = marker.read_bytes()
        before = listing(destination)
        assert_rejected(tool, destination)
        assert listing(destination) == before, 'rejected destination must remain untouched'
        for marker, payload in sentinels.items():
            assert marker.read_bytes() == payload, 'existing outputs must not be unlinked'


def external_into_tree(root: Path) -> None:
    tool = root / 'tools/tls/synthetic_credential.py'
    inside = root / 'src'
    with tempfile.TemporaryDirectory(prefix='tls-generation-') as directory:
        link = Path(directory) / 'into-tree'
        link.symlink_to(inside)
        before = listing(inside)
        assert_rejected(tool, link)
        assert link.is_symlink() and link.resolve() == inside.resolve(), link
        assert listing(inside) == before
        assert not (inside / CERT_NAME).exists() and not (inside / KEY_NAME).exists()


def external_tmp(root: Path) -> None:
    tool = root / 'tools/tls/synthetic_credential.py'
    with tempfile.TemporaryDirectory(prefix='tls-generation-') as directory:
        destination = Path(directory)
        result = run(tool, destination)
        assert result.returncode == 0, result
        assert 'synthetic-credential-generated neutral-dn self-signed' in result.stdout, result
        assert (destination / CERT_NAME).is_file() and (destination / KEY_NAME).is_file()


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    case = sys.argv[2]
    match case:
        case 'repository-root':
            repository_root(root)
        case 'source-subdirectory':
            source_subdirectory(root)
        case 'external-into-tree':
            external_into_tree(root)
        case 'external-tmp':
            external_tmp(root)
        case unknown:
            raise AssertionError(f'unknown generation case: {unknown}')
    print(f'synthetic-credential-containment: PASS {case}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
