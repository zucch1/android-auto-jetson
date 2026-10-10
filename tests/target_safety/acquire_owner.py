# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/acquire_owner.py
"""Local regressions for GCC linker closure and dpkg directory aliases."""
from __future__ import annotations

import json
import os
import sys
import tempfile
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import target_safety.acquire_input_select as selector
from target_safety.acquire_models import PackageInfo, TargetIdentity
from target_safety.kernel import Blocked

PACKAGE = PackageInfo("libgcc-13-dev:arm64", "13.3.0-fixture", "arm64", "gcc-13")


def test_selection_when_gcc_script_requires_static_archive() -> None:
    # Given the GCC13 ARM64 script and owned archive, but no libgcc.so.
    with tempfile.TemporaryDirectory(prefix="acq-owner-", dir="/tmp/opencode") as tmp:
        base = Path(tmp)
        gcc = base / "usr/lib/gcc/aarch64-linux-gnu/13"
        gcc.mkdir(parents=True)
        script = gcc / "libgcc_s.so"
        archive = gcc / "libgcc.a"
        runtime = gcc / "libgcc_s.so.1"
        script.write_text("/* GNU ld script */\nGROUP ( libgcc_s.so.1 -lgcc )\n")
        archive.write_bytes(b"!<arch>\n")
        runtime.write_bytes(b"\x7fELF")
        policy = base / "policy.json"
        policy.write_text(json.dumps({"schema": "aa-acquire-input-policy/1",
            "package_rules": ["libgcc-13-dev"], "header_roots": [],
            "lib_dirs": [str(base / "usr/lib")], "linker_script_markers": ["GROUP("]}))
        with patch.object(selector, "dpkg_query", return_value=PACKAGE), \
                patch.object(selector, "dpkg_list_files", return_value=tuple(
                    str(p) for p in (gcc, script, archive, runtime))), \
                patch.object(selector, "dpkg_owner", return_value=None), \
                patch.object(selector, "live_identity", return_value=TargetIdentity(None, None, None, ())):
            # When the full selector closes the linker script.
            observed = selector.select_inputs(policy)
        # Then the owned archive resolves -lgcc, never a synthesized libgcc.so.
        assert {s.path for s in observed.specs} == {str(script), str(archive), str(runtime)}
        assert observed.packages[str(archive)] == PACKAGE


def test_owner_when_dpkg_lists_usrmerge_or_gcc_directory_alias() -> None:
    # Given an architecture-qualified owner and a listing through a directory alias.
    with tempfile.TemporaryDirectory(prefix="acq-alias-", dir="/tmp/opencode") as tmp:
        base = Path(tmp)
        real = base / "usr/lib/gcc/aarch64-linux-gnu/13"
        real.mkdir(parents=True)
        leaf = real / "libgcc.a"
        leaf.write_bytes(b"!<arch>\n")
        alias = base / "lib"
        os.symlink(base / "usr/lib", alias)
        listed = alias / "gcc/aarch64-linux-gnu/13/libgcc.a"
        with patch.object(selector, "dpkg_query", return_value=PACKAGE), \
                patch.object(selector, "dpkg_list_files", return_value=(str(listed),)):
            # When looking up the real linker closure path in the cached index.
            owner = selector.PackageIndex(("libgcc-13-dev",)).owner(str(leaf))
        # Then directory normalization retains the exact dpkg package metadata.
        assert owner == PACKAGE


def test_owner_when_symlink_and_referent_have_different_packages() -> None:
    # Given a development symlink and its separately owned runtime referent.
    with tempfile.TemporaryDirectory(prefix="acq-link-owner-", dir="/tmp/opencode") as tmp:
        base = Path(tmp)
        runtime = base / "libgcc_s.so.1"
        runtime.write_bytes(b"\x7fELF")
        link = base / "libgcc_s.so"
        os.symlink(runtime.name, link)
        runtime_owner = PackageInfo("libgcc-s1:arm64", "fixture", "arm64", "gcc-13")
        with patch.object(selector, "dpkg_query", side_effect=(PACKAGE, runtime_owner)), \
                patch.object(selector, "dpkg_list_files", side_effect=((str(link),), (str(runtime),))):
            # When resolving each literal path through the index.
            index = selector.PackageIndex(("libgcc-13-dev", "libgcc-s1"))
        # Then canonicalizing directories must not transfer the leaf link's owner.
        assert index.owner(str(link)) == PACKAGE
        assert index.owner(str(runtime)) == runtime_owner
        assert index.owner(str(base / "unowned")) is None


def test_closure_when_shared_library_precedes_archive_in_same_directory() -> None:
    # Given both shared and static forms in the script directory.
    with tempfile.TemporaryDirectory(prefix="acq-search-", dir="/tmp/opencode") as tmp:
        base = Path(tmp)
        script = base / "libgcc_s.so"
        script.write_text("GROUP ( -lgcc )")
        shared = base / "libgcc.so"
        archive = base / "libgcc.a"
        shared.write_bytes(b"\x7fELF")
        archive.write_bytes(b"!<arch>\n")
        # When resolving the linker-script closure.
        result = selector._closure({str(script)}, selector.Policy((), (), (), ()))
        # Then the dynamic linker preference is preserved.
        assert result == {str(script), str(shared)}


def test_selection_when_linker_closure_owner_is_unknown() -> None:
    # Given an owned script referring to an existing unowned archive.
    with tempfile.TemporaryDirectory(prefix="acq-unowned-", dir="/tmp/opencode") as tmp:
        base = Path(tmp)
        script = base / "libgcc_s.so"
        archive = base / "libgcc.a"
        script.write_text("GROUP ( -lgcc )")
        archive.write_bytes(b"!<arch>\n")
        policy = base / "policy.json"
        policy.write_text(json.dumps({"schema": "aa-acquire-input-policy/1",
            "package_rules": ["libgcc-13-dev"], "header_roots": [],
            "lib_dirs": [str(base)], "linker_script_markers": ["GROUP("]}))
        with patch.object(selector, "dpkg_query", return_value=PACKAGE), \
                patch.object(selector, "dpkg_list_files", return_value=(str(script),)), \
                patch.object(selector, "dpkg_owner", return_value=None):
            # When closing and checking ownership, Then unknown inputs fail closed.
            try:
                selector.select_inputs(policy)
            except Blocked as error:
                assert error.reason == "selection_owner_unresolved"
                assert error.detail == str(archive)
            else:
                raise AssertionError("unknown owner accepted")


if __name__ == "__main__":
    for test in (test_selection_when_gcc_script_requires_static_archive,
                 test_owner_when_dpkg_lists_usrmerge_or_gcc_directory_alias,
                 test_owner_when_symlink_and_referent_have_different_packages,
                 test_closure_when_shared_library_precedes_archive_in_same_directory,
                 test_selection_when_linker_closure_owner_is_unknown):
        test()
        print("PASS", test.__name__)
