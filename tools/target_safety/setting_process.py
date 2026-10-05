"""Five-second total setting subprocess boundary, including failure recovery."""
from __future__ import annotations

import contextlib
import os
import signal
import subprocess
import time
from typing import Final

SETTING_TIMEOUT_SECONDS: Final = 5.0


class SettingError(RuntimeError):
    """Typed setting failure owned by the launch/I/O boundary."""

    def __init__(self, operation: str, detail: str) -> None:
        self.operation = operation
        self.detail = detail
        super().__init__(operation, detail)

    def __str__(self) -> str:
        return f"{self.operation}: {self.detail}"


def execute(argv: tuple[str, ...], absolute_deadline: float) -> subprocess.CompletedProcess[str]:
    """Reserve recovery time inside, not in addition to, the operation deadline."""
    deadline = min(absolute_deadline, time.monotonic() + SETTING_TIMEOUT_SECONDS)
    if deadline <= time.monotonic():
        raise SettingError("budget_exhausted", f"{argv}: setting operation has no remaining time")
    proc: subprocess.Popen[str] | None = None
    completed = False
    try:
        proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace",
                                start_new_session=True,
                                env=dict(os.environ, LC_ALL="C"))
        remaining = max(0.0, deadline - time.monotonic())
        out, err = proc.communicate(timeout=max(0.0, remaining - min(0.2, remaining / 2)))
        if time.monotonic() >= deadline:
            raise SettingError("budget_exhausted", f"{argv}: setting operation completed beyond deadline")
        completed = True
        return subprocess.CompletedProcess(argv, proc.returncode, out, err)
    except subprocess.TimeoutExpired as error:
        raise SettingError("timeout", f"{argv} exceeded total operation allowance") from error
    except OSError as error:
        raise SettingError("launch_io", f"{argv}: {error}") from error
    finally:
        if proc is not None:
            try:
                if not completed:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(proc.pid, signal.SIGKILL)
                if proc.poll() is None:
                    try:
                        proc.wait(timeout=max(0.0, deadline - time.monotonic()))
                    except subprocess.TimeoutExpired as error:
                        raise SettingError("reap_timeout", f"{argv}: stop unconfirmed within total deadline") from error
            except OSError as error:
                raise SettingError("launch_io", f"{argv}: cleanup {error}") from error
            finally:
                for stream in (proc.stdout, proc.stderr):
                    if stream is not None:
                        try:
                            stream.close()
                        except OSError as error:
                            raise SettingError("launch_io", f"{argv}: close {error}") from error
