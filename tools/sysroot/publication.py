# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Imported by tools.sysroot.stage on Linux.
"""Atomic Linux directory publication that cannot replace even an empty existing version."""
from __future__ import annotations

import ctypes
import errno

from .models import Code, SysrootError, Version


def publish(parent: int, scratch: str, version: Version) -> None:
    """RENAME_NOREPLACE closes the check/rename race; unsupported hosts fail closed."""
    library = ctypes.CDLL(None, use_errno=True)
    try:
        rename = library.renameat2
    except AttributeError as error:
        raise SysrootError(Code.LOCAL_IO, 'renameat2 unavailable') from error
    rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_uint]
    rename.restype = ctypes.c_int
    result: int = rename(parent, scratch.encode(), parent, version.encode(), 1)
    if result != 0:
        number = ctypes.get_errno()
        if number == errno.EEXIST:
            raise SysrootError(Code.NEW_VERSION_REQUIRED, version)
        raise SysrootError(Code.LOCAL_IO, f'renameat2:{number}')
