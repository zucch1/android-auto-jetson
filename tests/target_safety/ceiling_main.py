# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_main.py
"""Exact supervisor main() entrypoint qualification through local test seams.

The seams inject a fake setting (fake sudo commands) and a fake bounded guard;
they cannot alter the production remote payload (built in stage1.supervisor_bundle)
or the target authority (the fixed setting). No real kernel write, no SSH.
"""
from __future__ import annotations

import io
import sys
import tempfile
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.setting import CEILING_ORIGINAL, CEILING_TEMPORARY, SettingError, SudoSetting
from target_safety.supervisor import Supervisor, main as supervisor_main
from ceiling_fixtures import MARKER, guard_argv
from ceiling_setting_fixtures import fake_setting


def _fake_sudo_setting(tmp: Path, write_ok: bool = True) -> tuple[SudoSetting, Path]:
    return fake_setting(tmp, write_ok)


def main_happy_path_restores() -> None:
    """Given fake sudo commands + fake guard, When main() runs, Then accept and restore."""
    tmp = Path(tempfile.mkdtemp(prefix="ceiling-main-", dir="/tmp/opencode"))
    setting, state = _fake_sudo_setting(tmp)
    out = io.StringIO()
    with redirect_stdout(out):
        code = supervisor_main(setting=setting, guard_argv=guard_argv(tmp, "success"), guard_stdin=b"")
    assert code == 0, out.getvalue()
    assert state.read_text() == str(CEILING_ORIGINAL)
    assert CEILING_TEMPORARY != CEILING_ORIGINAL
    print(f"PASS main() happy path with fake sudo commands + fake guard; fixture={tmp}")


def write_preflight_fails_closed() -> None:
    """Given no restore-write authority, When main() runs, Then fail closed and never raise."""
    tmp = Path(tempfile.mkdtemp(prefix="ceiling-main-denied-", dir="/tmp/opencode"))
    setting, state = _fake_sudo_setting(tmp, write_ok=False)
    out = io.StringIO()
    with redirect_stdout(out):
        code = supervisor_main(setting=setting, guard_argv=guard_argv(tmp, "success"), guard_stdin=b"")
    assert code == 70, out.getvalue()
    assert state.read_text() == str(CEILING_ORIGINAL), "must not mutate without write authority"
    assert "preflight" in out.getvalue() and "preflight_write_unauthorized" in out.getvalue()
    print(f"PASS main() write-preflight fail-closed (read auth is not write auth); fixture={tmp}")


def sudo_setting_write_probe_authorization() -> None:
    """Given a sudo policy query, When probing both directions, Then gate on authorization."""
    tmp = Path(tempfile.mkdtemp(prefix="ceiling-probe-", dir="/tmp/opencode"))
    allowed, _ = _fake_sudo_setting(tmp, write_ok=True)
    allowed.preflight()
    denied, _ = _fake_sudo_setting(tmp, write_ok=False)
    try:
        denied.preflight()
    except SettingError as error:
        assert error.operation == "preflight_write_unauthorized"
    else:
        raise AssertionError("unauthorized write direction accepted")
    print(f"PASS SudoSetting preflight probes both write directions non-mutatively; fixture={tmp}")


def event_parse_rejects_spoof_and_malformed() -> None:
    """Given spoof/malformed/valid final events, When parsed, Then accept only a real event."""
    tmp = Path(tempfile.mkdtemp(prefix="ceiling-events-", dir="/tmp/opencode"))
    setting, state = _fake_sudo_setting(tmp)
    with redirect_stdout(io.StringIO()):
        spoof = supervisor_main(setting=setting, guard_argv=guard_argv(tmp, "spoof"), guard_stdin=b"")
        malformed = supervisor_main(setting=setting, guard_argv=guard_argv(tmp, "malformed"), guard_stdin=b"")
        valid = supervisor_main(setting=setting, guard_argv=guard_argv(tmp, "success"), guard_stdin=b"")
    assert spoof == 70 and malformed == 70 and valid == 0
    assert state.read_text() == str(CEILING_ORIGINAL)
    print(f"PASS event parsing rejects text spoof and malformed final event; fixture={tmp}")


def event_forwarding_preserves_guard_events() -> None:
    """Given a guard with events/stderr, When main() runs, Then forward and preserve them."""
    tmp = Path(tempfile.mkdtemp(prefix="ceiling-forward-", dir="/tmp/opencode"))
    setting, _ = _fake_sudo_setting(tmp)
    out = io.StringIO()
    with redirect_stdout(out):
        supervisor_main(setting=setting, guard_argv=guard_argv(tmp, "success"), guard_stdin=b"")
    text = out.getvalue()
    assert f'"event": "guard_event"' in text or "guard_event" in text
    assert MARKER in text
    print(f"PASS parsed guard event forwarding preserves the exact final event; fixture={tmp}")


def setup_budget_exhaustion_blocks_guard() -> None:
    """Given a tiny total budget, When setup overruns, Then block before the guard runs."""
    tmp = Path(tempfile.mkdtemp(prefix="ceiling-budget-", dir="/tmp/opencode"))
    setting, state = _fake_sudo_setting(tmp)
    supervisor = Supervisor(
        setting,
        guard_argv(tmp, "success"),
        guard_stdin=b"",
        budget_seconds=0.4,
        cleanup_headroom_seconds=0.35,
    )
    outcome = supervisor.run()
    assert not outcome.accepted and outcome.guard_exit is None
    assert any("setup_budget_exhausted" in e or "timeout" in e for e in outcome.errors)
    assert state.read_text() == str(CEILING_ORIGINAL)
    print(f"PASS setup budget exhaustion reserves cleanup headroom and blocks the guard; fixture={tmp}")


def main() -> int:
    main_happy_path_restores()
    write_preflight_fails_closed()
    sudo_setting_write_probe_authorization()
    event_parse_rejects_spoof_and_malformed()
    event_forwarding_preserves_guard_events()
    setup_budget_exhaustion_blocks_guard()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
