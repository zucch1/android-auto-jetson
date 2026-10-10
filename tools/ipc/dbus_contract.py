#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/ipc/dbus_contract.py check --xml docs/dbus/org.custom.AndroidAutoReceiver1.xml
"""D-Bus control contract schema validation and codegen (task 21).

The checked schema below is the machine-readable contract model for
org.custom.AndroidAutoReceiver1: exactly the twelve control/status methods,
five events and the SemVer metadata from the plan todo 21. The introspection
XML (docs/dbus/org.custom.AndroidAutoReceiver1.xml) is the contract authority;
this tool validates it against the checked schema and, in ``generate`` mode,
embeds it into a generated C++ header that the runtime serves as its
introspection. Malformed XML, an unknown major version, any drift from the
checked schema, and any high-rate/audio/video payload type fail with a named
``DBUS_CONTRACT_*`` diagnostic on stdout and exit 1 (nothing is generated).
Exit 0 prints a ``dbus-contract PASS`` summary; exit 2 is usage error.
"""
from __future__ import annotations

import argparse
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Final

sys.path.insert(0, str(Path(__file__).resolve().parent))

from dbus_contract_schema import (
    ALLOWED_MAP_TYPE,
    ALLOWED_SCALAR_TYPES,
    INTERFACE,
    INTERFACE_PREFIX,
    MAJOR,
    METHODS,
    OBJECT_PATH,
    PROPERTIES,
    SEMVER,
    SEMVER_ANNOTATION,
    SIGNALS,
)

# One argument must declare exactly one complete D-Bus type from this set.
ALLOWED_SINGLE_TYPES: Final[frozenset[str]] = ALLOWED_SCALAR_TYPES | {ALLOWED_MAP_TYPE}


@dataclass(frozen=True, slots=True)
class Violation:
    code: str
    detail: str

    def render(self) -> str:
        return f"{self.code} {self.detail}"


def interface_major(name: str) -> int | None:
    match = MAJOR.match(name)
    return int(match.group(1)) if match else None


def is_high_rate(type_str: str) -> bool:
    """Bulk, binary or fd payload shapes: byte arrays and any array other than
    the string map, unix fds, variants, structs and dict entries. The contract
    allows only one scalar type or the string map per argument, so anything
    shaped for audio/video/high-rate data lands here."""
    return (type_str.startswith("a") and type_str != ALLOWED_MAP_TYPE) \
        or type_str.startswith(("(", "{")) or type_str in {"h", "v"}


def check_members(interface: ET.Element) -> list[Violation]:
    """Validates every ORIGINAL member and argument before any lookup table is
    built: duplicates are detected on the raw lists (never masked by dict
    collapse), argument directions must be declared or default correctly, and
    every argument's type is inspected independently of its direction."""
    violations: list[Violation] = []

    def record(code: str, detail: str) -> None:
        violations.append(Violation(code, detail))

    def check_type(kind: str, name: str, type_str: str) -> None:
        if not type_str:
            record("DBUS_CONTRACT_BAD_SIGNATURE", f"{kind} {name} argument without type")
            return
        if type_str in ALLOWED_SINGLE_TYPES:
            return
        if is_high_rate(type_str):
            record("DBUS_CONTRACT_HIGH_RATE_PAYLOAD",
                   f"{kind} {name} carries type '{type_str}'")
            return
        record("DBUS_CONTRACT_BAD_SIGNATURE",
               f"{kind} {name} argument must declare exactly one complete type, got '{type_str}'")

    def member_args(node: ET.Element, kind: str, name: str) -> tuple[str, str]:
        in_types: list[str] = []
        out_types: list[str] = []
        for arg in node.findall("arg"):
            type_str = arg.get("type", "")
            check_type(kind, name, type_str)
            declared = arg.get("direction")
            if kind == "method":
                direction = declared if declared is not None else "in"
                if direction not in ("in", "out"):
                    record("DBUS_CONTRACT_BAD_DIRECTION",
                           f"method {name} argument direction '{declared}'")
                    continue
                (in_types if direction == "in" else out_types).append(type_str)
            else:
                if declared is not None:
                    record("DBUS_CONTRACT_BAD_DIRECTION",
                           f"signal {name} argument direction '{declared}'")
                    continue
                in_types.append(type_str)
        return "".join(in_types), "".join(out_types)

    groups = {
        "method": interface.findall("method"),
        "signal": interface.findall("signal"),
        "property": interface.findall("property"),
    }

    seen: set[tuple[str, str]] = set()
    for kind, nodes in groups.items():
        for node in nodes:
            name = node.get("name", "")
            if (kind, name) in seen:
                record("DBUS_CONTRACT_DUPLICATE_MEMBER", f"{kind} {name}")
            seen.add((kind, name))

    signatures: dict[str, dict[str, tuple[str, str]]] = {"method": {}, "signal": {}}
    for kind in ("method", "signal"):
        for node in groups[kind]:
            name = node.get("name", "")
            signatures[kind].setdefault(name, member_args(node, kind, name))

    for name, expected in METHODS.items():
        got = signatures["method"].get(name)
        if got is None:
            record("DBUS_CONTRACT_MISSING_METHOD", name)
            continue
        if got != expected:
            record("DBUS_CONTRACT_BAD_SIGNATURE",
                   f"{name} expected {expected} got {got}")
    for name in signatures["method"]:
        if name not in METHODS:
            record("DBUS_CONTRACT_UNEXPECTED_METHOD", name)

    for name, expected in SIGNALS.items():
        got = signatures["signal"].get(name)
        if got is None:
            record("DBUS_CONTRACT_MISSING_SIGNAL", name)
            continue
        if got[0] != expected:
            record("DBUS_CONTRACT_SIGNAL_MISMATCH",
                   f"{name} expected '{expected}' got '{got[0]}'")
    for name in signatures["signal"]:
        if name not in SIGNALS:
            record("DBUS_CONTRACT_UNEXPECTED_SIGNAL", name)

    for node in groups["property"]:
        name = node.get("name", "")
        got = node.get("type", "")
        check_type("property", name, got)
        expected = PROPERTIES.get(name)
        if expected is None:
            record("DBUS_CONTRACT_UNEXPECTED_PROPERTY", name)
            continue
        if got != expected or node.get("access") != "read":
            record("DBUS_CONTRACT_PROPERTY_MISMATCH",
                   f"{name} expected read-only '{expected}' got '{got}/{node.get('access')}'")
    for name in PROPERTIES:
        if not any(node.get("name") == name for node in groups["property"]):
            record("DBUS_CONTRACT_MISSING_PROPERTY", name)

    for child in interface:
        if child.tag not in {"method", "signal", "property", "annotation"}:
            record("DBUS_CONTRACT_UNEXPECTED_ELEMENT", f"interface child <{child.tag}>")
    return violations


def check_semver(interface: ET.Element) -> list[Violation]:
    annotations = {a.get("name", ""): a.get("value", "") for a in interface.findall("annotation")}
    value = annotations.get(SEMVER_ANNOTATION)
    if value is None:
        return [Violation("DBUS_CONTRACT_MISSING_SEMVER", SEMVER_ANNOTATION)]
    if value != SEMVER:
        return [Violation("DBUS_CONTRACT_SEMVER_MISMATCH",
                          f"{SEMVER_ANNOTATION} expected '{SEMVER}' got '{value}'")]
    parts = value.split(".")
    if len(parts) != 3 or not all(p.isdigit() for p in parts):
        return [Violation("DBUS_CONTRACT_SEMVER_MISMATCH", f"'{value}' is not MAJOR.MINOR.PATCH")]
    if int(parts[0]) != interface_major(interface.get("name", "")):
        return [Violation("DBUS_CONTRACT_SEMVER_MISMATCH",
                          f"semver major {parts[0]} != interface-name major")]
    return []


def check_document(text: str) -> tuple[list[Violation], ET.Element | None]:
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        return [Violation("DBUS_CONTRACT_MALFORMED_XML", str(exc))], None
    violations: list[Violation] = []
    if root.tag != "node":
        violations.append(Violation("DBUS_CONTRACT_UNEXPECTED_ELEMENT", f"root <{root.tag}>"))
        return violations, None
    if root.get("name") != OBJECT_PATH:
        violations.append(Violation("DBUS_CONTRACT_WRONG_OBJECT_PATH",
                                    f"expected {OBJECT_PATH} got {root.get('name')}"))
    interfaces = root.findall("interface")
    if len(interfaces) != 1:
        violations.append(Violation("DBUS_CONTRACT_UNEXPECTED_ELEMENT",
                                    f"expected 1 interface got {len(interfaces)}"))
        return violations, None
    interface = interfaces[0]
    name = interface.get("name", "")
    if name != INTERFACE:
        major = interface_major(name)
        if major is not None and major != 1:
            violations.append(Violation("DBUS_CONTRACT_UNSUPPORTED_MAJOR",
                                        f"{name} (supported: {INTERFACE})"))
        else:
            violations.append(Violation("DBUS_CONTRACT_WRONG_INTERFACE", name))
        return violations, None
    violations.extend(check_semver(interface))
    violations.extend(check_members(interface))
    return violations, root


def xml_octal_chunks(text: str, chunk_bytes: int = 24) -> list[str]:
    """Encodes the XML text as fixed-width three-digit octal escapes so no
    input byte sequence (quotes, backslashes, newlines, delimiter look-alikes,
    arbitrary UTF-8) can collide with the C++ string-literal syntax."""
    escapes = [f"\\{byte:03o}" for byte in text.encode("utf-8")]
    return ["".join(escapes[index:index + chunk_bytes])
            for index in range(0, len(escapes), chunk_bytes)]


def generate(text: str, root: ET.Element) -> str:
    interface = root.find("interface")
    assert interface is not None

    def quoted(items: list[str]) -> str:
        return ", ".join(f'"{n}"' for n in items)

    method_names = list(METHODS)
    method_in = [METHODS[name][0] for name in method_names]
    method_out = [METHODS[name][1] for name in method_names]
    signal_names = list(SIGNALS)
    property_names = list(PROPERTIES)
    literal = "\n".join(f'    "{chunk}"' for chunk in xml_octal_chunks(text))
    byte_length = len(text.encode("utf-8"))

    return f"""// SPDX-License-Identifier: GPL-3.0-or-later
// GENERATED by tools/ipc/dbus_contract.py from the checked D-Bus contract
// XML. Do not edit; regenerate via the aa_dbus_contract_codegen target.
#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace aa::ipc::dbus_contract {{

inline constexpr char kIntrospectionXmlLiteral[] =
{literal};
inline constexpr std::size_t kIntrospectionXmlBytes = {byte_length};
static_assert(sizeof(kIntrospectionXmlLiteral) - 1 == kIntrospectionXmlBytes);
inline constexpr std::string_view kIntrospectionXml{{kIntrospectionXmlLiteral,
                                                    kIntrospectionXmlBytes}};
inline constexpr std::string_view kInterfaceName = "{INTERFACE}";
inline constexpr std::string_view kObjectPath = "{OBJECT_PATH}";
inline constexpr std::string_view kContractSemVer = "{SEMVER}";
inline constexpr std::array<std::string_view, {len(method_names)}> kMethodNames{{{{ {quoted(method_names)} }}}};
inline constexpr std::array<std::string_view, {len(method_in)}> kMethodInSignatures{{{{ {quoted(method_in)} }}}};
inline constexpr std::array<std::string_view, {len(method_out)}> kMethodOutSignatures{{{{ {quoted(method_out)} }}}};
inline constexpr std::array<std::string_view, {len(signal_names)}> kSignalNames{{{{ {quoted(signal_names)} }}}};
inline constexpr std::array<std::string_view, {len(property_names)}> kPropertyNames{{{{ {quoted(property_names)} }}}};

}}  // namespace aa::ipc::dbus_contract
"""


def main() -> int:
    parser = argparse.ArgumentParser(description="validate/generate the D-Bus control contract")
    parser.add_argument("mode", choices=("check", "generate"))
    parser.add_argument("--xml", required=True, type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()

    if args.mode == "generate" and args.out is None:
        print("usage: generate requires --out", file=sys.stderr)
        return 2
    try:
        # Byte-preserving read: read_text() normalizes newlines, which would
        # contradict the byte-for-byte introspection/embedding comparison.
        text = args.xml.read_bytes().decode("utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        print(f"DBUS_CONTRACT_UNREADABLE {exc}", file=sys.stderr)
        return 2

    violations, root = check_document(text)
    if violations:
        for violation in violations:
            print(f"dbus-contract FAIL {violation.render()}")
        return 1

    assert root is not None
    if args.mode == "generate":
        assert args.out is not None
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(generate(text, root), encoding="utf-8")
    print(f"dbus-contract PASS interface={INTERFACE} methods={len(METHODS)} "
          f"signals={len(SIGNALS)} properties={len(PROPERTIES)} semver={SEMVER}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
