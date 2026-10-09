"""Fail-closed interpretation of a deliberately narrow sudo -ll policy grammar.

Two fully consumed shapes only: the explicit root passwordless command pair,
or the observed ordered ALL entries with exactly the qualified Defaults.
This is not a general policy engine or permission to broaden write operations.
The observed profile assumes ordinary sudoers from one file-backed tree:
display forward, lookup reverse, standard last-match semantics. The sudo version
and backend are unverified; source labels narrow the profile, neither confer
authority nor prove that backend. Fresh root command probes remain mandatory.
"""
from __future__ import annotations

import re
from typing import Final

from .setting_process import SettingError

_DEFAULTS: Final = (
    r"env_reset, mail_badpass, secure_path=/usr/local/sbin\:/usr/local/bin\:/usr/sbin"
    r"\:/usr/bin\:/sbin\:/bin\:/snap/bin, use_pty"
)
# Under the single-tree contract above, the final matching ALL rule supplies
# !authenticate. Full consumption fixes both source labels and rule order.
_OBSERVED: Final = re.compile(
    r"Matching Defaults entries for (?P<user>[\w.-]+) on (?P<host>[\w.-]+):\n"
    + re.escape(_DEFAULTS) + r"\n"
    + r"User (?P=user) may run the following commands on (?P=host):\n"
    + r"Sudoers entry: /etc/sudoers\n"
    + r"RunAsUsers: ALL\nRunAsGroups: ALL\nCommands:\nALL\n"
    + r"Sudoers entry: /etc/sudoers\.d/90-cloud-init-users\n"
    + r"RunAsUsers: ALL\nOptions: !authenticate\nCommands:\nALL",
    re.ASCII,
)


def require_passwordless(raw: str, commands: tuple[str, str]) -> None:
    """Qualify either supported shape after fresh exact-command permission probes."""
    lines = [line.strip() for line in raw.splitlines() if line.strip()]
    if _OBSERVED.fullmatch("\n".join(lines)):
        return
    if lines and re.fullmatch(r"User [\w.-]+ may run the following commands on [\w.-]+:", lines[0]):
        lines = lines[1:]
    prefix = ["Sudoers entry:", "RunAsUsers: root", "Options: !authenticate", "Commands:"]
    if lines[:4] != prefix or len(lines) != 6 or set(lines[4:]) != set(commands):
        raise SettingError("preflight_passwordless_unproven",
                           "unsupported, PASSWD/cache-only or ambiguous policy; qualified root NOPASSWD shape required")
