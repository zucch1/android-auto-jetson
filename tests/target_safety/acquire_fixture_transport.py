# SPDX-License-Identifier: GPL-3.0-or-later
"""Local Python executable adapter for deterministic proc/dpkg fixture readers."""
from __future__ import annotations

import os
import sys
from contextlib import contextmanager
from collections.abc import Iterator
from pathlib import Path
from unittest.mock import patch


@contextmanager
def fixture_transport(base: Path, live: bool = False) -> Iterator[None]:
    bindir = base / "bin"
    bindir.mkdir()
    executable = bindir / "python3"
    executable.write_text(f"#!{sys.executable}\n" + '''
import dataclasses, json, os, sys
from pathlib import Path
import target_safety.acquire as a
import target_safety.acquire_input_select as s
from target_safety.acquire_models import ProcessObservation, PackageInfo, TargetIdentity
a.observe_processes = lambda: ProcessObservation((("1", "fixture-init", "/"),), None)
if os.environ.get("ACQ_FIXTURE_LIVE"):
    base = Path(os.environ["ACQ_FIXTURE_BASE"])
    original = s.load_policy
    def policy(path=s.POLICY_PATH):
        p = original(path)
        return dataclasses.replace(p, header_roots=(str(base / "tgt/usr/include"),),
                                    lib_dirs=(str(base / "tgt/usr/lib"),))
    s.load_policy = policy
    pkg = PackageInfo("libfixture", "1.0-1", "arm64", "fixture-src")
    s.dpkg_query = lambda name: pkg if name == "libc6-dev" else None
    s.dpkg_list_files = lambda name: tuple(str(p) for p in (base / "tgt").rglob("*"))
    s.dpkg_owner = lambda path: pkg
    s.live_identity = lambda: TargetIdentity("fixture-kernel", "fixture-l4t", None, ())
    original_select = s.select_inputs
    def select():
        value = original_select()
        return dataclasses.replace(value, provenance="fixture")
    s.select_inputs = select
raise SystemExit(a.main(sys.argv[4:]))
''')
    executable.chmod(0o700)
    with patch.dict(os.environ, {"PATH": str(bindir) + ":" + os.environ["PATH"],
        "ACQ_FIXTURE_LIVE": "1" if live else "", "ACQ_FIXTURE_BASE": str(base)}):
        yield
