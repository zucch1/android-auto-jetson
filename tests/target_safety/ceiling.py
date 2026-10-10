# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling.py
"""Ceiling supervisor boundary qualification: classify, reconcile, preflight, caps.

Synthetic cases (no real kernel mutation, no real subprocesses): the setting
boundary logic is exercised against an in-process fake executor. Process
lifecycle is covered separately in ceiling_lifecycle.py. No sysctl CAS exists;
these cases prove the reconciliation policy, not a concurrency guarantee.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import setting
from target_safety.setting import (
    CEILING_ORIGINAL,
    CEILING_TEMPORARY,
    SettingError,
    classify,
    reconcile_raise,
    reconcile_restore,
)


class FakeSetting:
    """Mutable in-process fake setting; mutation is its documented purpose."""

    def __init__(self, value: int, deny_write: bool = False, deny_preflight: bool = False) -> None:
        self.value = value
        self.deny_write = deny_write
        self.deny_preflight = deny_preflight
        self.writes: list[int] = []

    def read(self, timeout: float | None = None, *, deadline: float | None = None) -> int:
        return self.value

    def write(self, value: int, timeout: float | None = None, *, deadline: float | None = None) -> None:
        if self.deny_write:
            raise SettingError("denied", "sudo -n refused write")
        self.writes.append(value)
        self.value = value

    def preflight(self, timeout: float | None = None, *, deadline: float | None = None) -> None:
        if self.deny_preflight:
            raise SettingError("preflight_denied", "sudo -n refused preflight")


def classify_partition() -> None:
    """Given observed values, When classified, Then partition original/temporary/conflict."""
    assert classify(CEILING_ORIGINAL) == "original"
    assert classify(CEILING_TEMPORARY) == "temporary"
    assert classify(65535) == "conflict"
    assert classify(1048575) == "conflict"
    assert classify(0) == "conflict"
    print("PASS ceiling classify partitions original/temporary/conflict")


def constants_exact() -> None:
    """Given the approved reversible pair, When read, Then equal 65536 and 1048576."""
    assert CEILING_ORIGINAL == 65536
    assert CEILING_TEMPORARY == 1048576
    assert (CEILING_ORIGINAL, CEILING_TEMPORARY) == (65536, 1048576)
    assert setting.CEILING_SETTING == "fs.inotify.max_user_watches"
    print("PASS ceiling constants original=65536 temporary=1048576 exact setting")


def reconcile_raise_disposition() -> None:
    """Given the value after the raise write, When reconciled, Then proceed/no-write/conflict."""
    assert reconcile_raise(CEILING_TEMPORARY).action == "proceed"
    assert reconcile_raise(CEILING_TEMPORARY).value_class == "temporary"
    assert reconcile_raise(CEILING_ORIGINAL).action == "no_write_raise_did_not_take"
    assert reconcile_raise(123456).action == "conflict_no_overwrite"
    assert reconcile_raise(123456).write_value is None
    print("PASS ceiling reconcile_raise proceed/no-write/conflict dispositions")


def reconcile_restore_conditional() -> None:
    """Given the observed pre-restore value, When reconciled, Then only temporary restores original."""
    temporary = reconcile_restore(CEILING_TEMPORARY)
    assert temporary.action == "restore_original"
    assert temporary.write_value == CEILING_ORIGINAL
    already = reconcile_restore(CEILING_ORIGINAL)
    assert already.action == "no_write_original_already"
    assert already.write_value is None
    # Observed conflict must never overwrite: no silent overwrite of a foreign value.
    conflict = reconcile_restore(131072)
    assert conflict.action == "conflict_no_overwrite"
    assert conflict.value_class == "conflict"
    assert conflict.write_value is None
    print("PASS ceiling reconcile_restore conditional; conflict never overwrites")


def preflight_guards_wrong_original_and_denied() -> None:
    """Given wrong original or denied sudo, When preflighting, Then refuse without mutating."""
    good = FakeSetting(CEILING_ORIGINAL)
    assert setting.preflight(good) == CEILING_ORIGINAL
    assert good.writes == []
    wrong = FakeSetting(CEILING_TEMPORARY)
    try:
        setting.preflight(wrong)
    except SettingError as error:
        assert error.operation == "preflight_wrong_original"
        assert wrong.writes == []
    else:
        raise AssertionError("wrong original accepted at preflight")
    denied = FakeSetting(CEILING_ORIGINAL, deny_preflight=True)
    try:
        setting.preflight(denied)
    except SettingError as error:
        assert error.operation == "preflight_denied"
        assert denied.writes == []
    else:
        raise AssertionError("denied preflight accepted")
    print("PASS ceiling preflight rejects wrong original and denied sudo without mutation")


def sudo_setting_refuses_foreign_write() -> None:
    """Given the bounded sudo executor, When writing a foreign value, Then refuse exactly."""
    s = setting.SudoSetting(sudo=("false",), timeout_seconds=1.0)
    try:
        s.write(999999)
    except SettingError as error:
        assert error.operation == "refused_write"
    else:
        raise AssertionError("foreign value write accepted")
    print("PASS ceiling SudoSetting restricts writes to the exact reversible pair")


def stdin_bundle_exposes_unchanged_caps() -> None:
    """Given the actual supervisor stdin bundle, When loaded, Then caps unchanged."""
    from target_safety import stage1

    expected = (f"ceiling-caps {CEILING_ORIGINAL} {CEILING_TEMPORARY} "
                f"{stage1.EFFECTIVE_CAPS['protected_entries']} "
                f"{stage1.EFFECTIVE_CAPS['registered_watches']} "
                f"{stage1.EFFECTIVE_CAPS['single_monotonic_window_seconds']} "
                f"{stage1.EFFECTIVE_CAPS['bytes_per_protected_inventory']}")
    entry = (
        "from target_safety.setting import CEILING_ORIGINAL, CEILING_TEMPORARY\n"
        "from target_safety.coverage import PROTECTED_ENTRY_LIMIT\n"
        "from target_safety.kernel import MAX_WATCHES, WINDOW_SECONDS\n"
        "from target_safety.inventory import INVENTORY_BYTE_LIMIT\n"
        "print('ceiling-caps', CEILING_ORIGINAL, CEILING_TEMPORARY, PROTECTED_ENTRY_LIMIT,"
        " MAX_WATCHES, WINDOW_SECONDS, INVENTORY_BYTE_LIMIT)\n"
    )
    payload = stage1.supervisor_bundle().rsplit(b"raise SystemExit", 1)[0] + entry.encode()
    result = subprocess.run([sys.executable, "-B", "-"], input=payload,
                            capture_output=True, check=False, timeout=20)
    assert result.returncode == 0, result.stderr
    assert expected.encode() in result.stdout, result.stdout
    print("PASS ceiling actual stdin bundle loads supervisor with unchanged caps")


def main() -> int:
    constants_exact()
    classify_partition()
    reconcile_raise_disposition()
    reconcile_restore_conditional()
    preflight_guards_wrong_original_and_denied()
    sudo_setting_refuses_foreign_write()
    stdin_bundle_exposes_unchanged_caps()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
