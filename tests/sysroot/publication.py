# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tests/sysroot/publication.py
"""Exercise native no-replace publication using actual local directories."""
from __future__ import annotations

from contextlib import ExitStack
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.sysroot.filesystem import directory
from tools.sysroot.models import Code, SysrootError, Version
from tools.sysroot.publication import publish


def main() -> int:
    with tempfile.TemporaryDirectory(prefix='sysroot-publish-') as temporary, ExitStack() as stack:
        # Given: a scratch tree and an existing, even empty version directory.
        parent = Path(temporary)
        (parent / 'scratch').mkdir()
        (parent / 'v1').mkdir()
        descriptor = directory(parent, stack)
        before = (parent / 'v1').stat()
        # When: native publication collides with the existing version.
        try:
            publish(descriptor, 'scratch', Version('v1'))
        except SysrootError as error:
            # Then: both directories are retained without replacement.
            assert error.code == Code.NEW_VERSION_REQUIRED
            assert (parent / 'v1').stat() == before
            assert (parent / 'scratch').is_dir()
        else:
            raise AssertionError('replaced existing version')
        print('PASS native-no-replace')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
