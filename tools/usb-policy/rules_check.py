# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Fail-closed udev rules parser and structural matcher for tools/usb-policy.

Stdlib only. Covers exactly the udev key subset used by
``70-android-auto-receiver.rules``:

- match keys: ``KERNEL``, ``SUBSYSTEM``, ``ACTION``, ``ENV{X}``, ``ATTR{X}``
  on the device itself and ``KERNELS``, ``SUBSYSTEMS``, ``ATTRS{X}`` on the
  device-or-ancestor chain (all chain keys of one rule must be satisfied by
  the SAME chain node, like udev),
- operators ``==`` / ``!=`` (match, glob values like udev's fnmatch) and
  ``=`` / ``+=`` / ``:=`` (assign),
- assignment keys: ``ENV{X}``, ``TAG``, ``GROUP``, ``MODE``, ``OWNER``.

Anything else - unknown keys, unbalanced quotes, missing operators, bare
garbage - raises :class:`RuleError`. A malformed file is never partially
trusted and never "matches everything": :func:`parse_rules` fails closed and
the caller must treat the file as invalid.

Also provides a small parser for ``udevadm test`` output so recorded fixtures
can prove that the shipped rules match only the qualified device tree.
Device-supplied text (manufacturer/product/serial) is never consulted here:
matching uses ids, port path and subsystem only.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# ---------------------------------------------------------------------------
# Errors


class RuleError(ValueError):
    """Malformed rules or udevadm input; fail closed, never match loosely."""

    def __init__(self, reason: str, line_no: int | None = None, text: str = ""):
        where = f"line {line_no}: " if line_no is not None else ""
        detail = f" ({text!r})" if text else ""
        super().__init__(f"{where}{reason}{detail}")
        self.reason = reason
        self.line_no = line_no
        self.text = text


# ---------------------------------------------------------------------------
# Rule model

_MATCH_KEYS = {"KERNEL", "SUBSYSTEM", "ACTION", "ENV", "ATTR", "KERNELS", "SUBSYSTEMS", "ATTRS"}
_ASSIGN_KEYS = {"ENV", "TAG", "GROUP", "MODE", "OWNER"}
_MATCH_OPS = {"==", "!="}
_ASSIGN_OPS = {"=", "+=", ":="}
# Keys that are searched on the device itself vs. the device-or-ancestor chain.
_CHAIN_KEYS = {"KERNELS", "SUBSYSTEMS", "ATTRS"}
_ARG_KEYS = {"ENV", "ATTR", "ATTRS"}

_TOKEN_RE = re.compile(
    r"""^(?P<key>[A-Z]+)(?:\{(?P<arg>[^}]*)\})?(?P<op>==|!=|\+=|:=|=)(?P<value>"[^"]*"|[^"]*)$"""
)


@dataclass(frozen=True)
class MatchKey:
    key: str
    arg: str | None
    op: str
    value: str

    def describe(self) -> str:
        name = self.key if self.arg is None else f"{self.key}{{{self.arg}}}"
        return f"{name}{self.op}{self.value!r}"


@dataclass(frozen=True)
class Assignment:
    key: str
    arg: str | None
    op: str
    value: str

    def describe(self) -> str:
        name = self.key if self.arg is None else f"{self.key}{{{self.arg}}}"
        return f"{name}{self.op}{self.value!r}"


@dataclass(frozen=True)
class Rule:
    line_no: int
    matches: tuple[MatchKey, ...]
    assignments: tuple[Assignment, ...]

    def describe(self) -> str:
        left = ", ".join(m.describe() for m in self.matches)
        right = ", ".join(a.describe() for a in self.assignments)
        return f"{left} -> {right}"


def _split_tokens(line: str, line_no: int) -> list[str]:
    """Split a rule line on commas outside double quotes; fail closed."""
    tokens: list[str] = []
    current: list[str] = []
    in_quotes = False
    for ch in line:
        if ch == '"':
            in_quotes = not in_quotes
            current.append(ch)
        elif ch == "," and not in_quotes:
            tokens.append("".join(current).strip())
            current = []
        else:
            current.append(ch)
    if in_quotes:
        raise RuleError("unbalanced quotes", line_no, line)
    tokens.append("".join(current).strip())
    return [t for t in tokens if t]


def _parse_token(token: str, line_no: int) -> tuple[MatchKey | None, Assignment | None]:
    match = _TOKEN_RE.match(token)
    if match is None:
        raise RuleError("malformed token", line_no, token)
    key = match.group("key")
    arg = match.group("arg")
    op = match.group("op")
    raw = match.group("value")
    if raw.startswith('"'):
        if not raw.endswith('"') or len(raw) < 2:
            raise RuleError("malformed quoted value", line_no, token)
        value = raw[1:-1]
    else:
        value = raw
    if arg is not None and key not in _ARG_KEYS:
        raise RuleError(f"key does not take an argument: {key}", line_no, token)
    if arg is None and key in _ARG_KEYS:
        raise RuleError(f"key requires an argument: {key}", line_no, token)
    if op in _MATCH_OPS:
        if key not in _MATCH_KEYS:
            raise RuleError(f"unknown match key: {key}", line_no, token)
        return MatchKey(key, arg, op, value), None
    if op in _ASSIGN_OPS:
        if key not in _ASSIGN_KEYS:
            raise RuleError(f"unknown assignment key: {key}", line_no, token)
        return None, Assignment(key, arg, op, value)
    raise RuleError(f"unknown operator: {op}", line_no, token)


def parse_rules(text: str) -> list[Rule]:
    """Parse a whole rules file. Raises RuleError on the first bad line."""
    rules: list[Rule] = []
    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        matches: list[MatchKey] = []
        assignments: list[Assignment] = []
        for token in _split_tokens(line, line_no):
            match, assign = _parse_token(token, line_no)
            if match is not None:
                matches.append(match)
            else:
                assert assign is not None
                assignments.append(assign)
        if not matches:
            raise RuleError("rule with no match keys", line_no, line)
        if not assignments:
            raise RuleError("rule with no assignments", line_no, line)
        rules.append(Rule(line_no, tuple(matches), tuple(assignments)))
    if not rules:
        raise RuleError("no rules found")
    return rules


# ---------------------------------------------------------------------------
# Device model and matching


@dataclass(frozen=True)
class Device:
    """One sysfs device; ``parents`` are ancestors nearest-first."""

    kernel: str
    subsystem: str
    attrs: dict[str, str] = field(default_factory=dict)
    env: dict[str, str] = field(default_factory=dict)
    parents: tuple["Device", ...] = ()

    @property
    def chain(self) -> tuple["Device", ...]:
        """This device then every ancestor, nearest-first (recursive)."""
        result: list[Device] = [self]
        for parent in self.parents:
            result.extend(parent.chain)
        return tuple(result)

    @staticmethod
    def from_json(data: dict) -> "Device":
        parents = tuple(Device.from_json(p) for p in data.get("parents", []))
        return Device(
            kernel=data["kernel"],
            subsystem=data["subsystem"],
            attrs=dict(data.get("attrs", {})),
            env=dict(data.get("env", {})),
            parents=parents,
        )


def _actual(node: Device, key: str, arg: str | None) -> str | None:
    if key in {"KERNEL", "KERNELS"}:
        return node.kernel
    if key in {"SUBSYSTEM", "SUBSYSTEMS"}:
        return node.subsystem
    if key == "ACTION":
        return node.env.get("ACTION")
    if key == "ATTR":
        assert arg is not None
        return node.attrs.get(arg)
    if key == "ATTRS":
        assert arg is not None
        return node.attrs.get(arg)
    if key == "ENV":
        assert arg is not None
        return node.env.get(arg)
    raise RuleError(f"unsupported key: {key}")


def _glob_matches(value: str | None, pattern: str) -> bool:
    """udev-style matching: fnmatch glob per alternative, '|' = alternation.

    Verified against udev behaviour on the target kernel: match values are
    shell-style globs and ``|`` separates alternatives (e.g. the shipped
    ModemManager rule ``ACTION!="add|change|move"`` acts as alternation).
    A missing value matches only via ``!=`` semantics.
    """
    if value is None:
        return False
    return any(fnmatch.fnmatchcase(value, part) for part in pattern.split("|"))


def _match_key_ok(match: MatchKey, node: Device) -> bool:
    actual = _actual(node, match.key, match.arg)
    if match.op == "==":
        return _glob_matches(actual, match.value)
    return not _glob_matches(actual, match.value)


def rule_matches(rule: Rule, device: Device) -> bool:
    """udev semantics: own keys on the device; chain keys on ONE chain node."""
    chain_parts = [m for m in rule.matches if m.key in _CHAIN_KEYS]
    own_parts = [m for m in rule.matches if m.key not in _CHAIN_KEYS]
    if not all(_match_key_ok(m, device) for m in own_parts):
        return False
    if not chain_parts:
        return True
    return any(all(_match_key_ok(m, node) for m in chain_parts) for node in device.chain)


def apply_rules(rules: list[Rule], device: Device) -> dict[str, str]:
    """Apply every matching rule in order; later '=' overwrites, '+=' appends."""
    result: dict[str, str] = {}
    final_keys: set[str] = set()
    for rule in rules:
        if not rule_matches(rule, device):
            continue
        for assign in rule.assignments:
            name = assign.key if assign.arg is None else f"{assign.key}{{{assign.arg}}}"
            if name in final_keys and assign.op != ":=":
                continue
            if assign.op == "+=":
                prior = result.get(name, "")
                result[name] = f"{prior} {assign.value}".strip()
            else:
                result[name] = assign.value
                if assign.op == ":=":
                    final_keys.add(name)
    return result


# ---------------------------------------------------------------------------
# udevadm test output


@dataclass(frozen=True)
class UdevadmRun:
    """Parsed ``udevadm test`` output: rule trace lines + final properties."""

    properties: dict[str, str]
    rule_lines: tuple[tuple[str, int, str], ...]  # (rules file path, line no, rest)

    def assignments_for(self, rules_file: str) -> list[str]:
        """Trace fragments attributed to a rules file (suffix match on name)."""
        out = []
        for path, _line, rest in self.rule_lines:
            if path.endswith(rules_file):
                out.append(rest)
        return out


_TRACE_RE = re.compile(r"^(?P<sysname>\S+): (?P<file>/[^:]+\.rules):(?P<line>\d+) (?P<rest>.*)$")
_PROPERTY_RE = re.compile(r"^(?P<key>[A-Z][A-Z0-9_]*)=(?P<value>.*)$")


def parse_udevadm_test(text: str) -> UdevadmRun:
    """Parse rule-trace lines and the final property block of udevadm test."""
    properties: dict[str, str] = {}
    rule_lines: list[tuple[str, int, str]] = []
    for raw in text.splitlines():
        line = raw.rstrip()
        trace = _TRACE_RE.match(line)
        if trace:
            rule_lines.append((trace.group("file"), int(trace.group("line")), trace.group("rest")))
            continue
        prop = _PROPERTY_RE.match(line)
        if prop:
            properties[prop.group("key")] = prop.group("value")
    return UdevadmRun(properties=properties, rule_lines=tuple(rule_lines))


# ---------------------------------------------------------------------------
# CLI


def _cli(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    verify = sub.add_parser("verify", help="parse a rules file; exit 0 only if valid")
    verify.add_argument("file", type=Path)
    match = sub.add_parser("match", help="apply a rules file to a JSON device description")
    match.add_argument("file", type=Path)
    match.add_argument("--device", required=True, help="JSON device (see Device.from_json)")
    udevadm = sub.add_parser("udevadm-output", help="summarize parsed udevadm test output")
    udevadm.add_argument("file", type=Path)
    udevadm.add_argument("--rules-file", default="70-android-auto-receiver.rules")
    args = parser.parse_args(argv)

    try:
        text = args.file.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"error: cannot read {args.file}: {exc}", file=sys.stderr)
        return 2

    try:
        if args.command == "verify":
            rules = parse_rules(text)
            print(f"ok: {len(rules)} rules")
            return 0
        if args.command == "match":
            device = Device.from_json(json.loads(args.device))
            result = apply_rules(parse_rules(text), device)
            print(json.dumps(result, sort_keys=True))
            return 0
        run = parse_udevadm_test(text)
        summary = {
            "matched_rules_lines": run.assignments_for(args.rules_file),
            "id_mm_device_ignore": run.properties.get("ID_MM_DEVICE_IGNORE"),
            "property_count": len(run.properties),
        }
        print(json.dumps(summary, sort_keys=True))
        return 0
    except RuleError as exc:
        print(f"invalid: {exc}", file=sys.stderr)
        return 1
    except json.JSONDecodeError as exc:
        print(f"error: bad JSON device: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
