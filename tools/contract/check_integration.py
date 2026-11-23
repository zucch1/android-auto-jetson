#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/contract/check_integration.py --root .
"""Verify docs/INTEGRATION-CONTRACT.md matches the in-tree code and packaging.

Checks:
  1. Contract SemVer header and required top-level sections exist.
  2. Receiver D-Bus XML (docs/dbus/org.custom.AndroidAutoReceiver1.xml),
     Contract.hpp runtime identity constants (bus name, object path, interface,
     major), BusWire.hpp max message size, and DispatchReply.hpp custom error
     names match the contract tables exactly.
  3. System Helper D-Bus XML (docs/dbus/org.custom.AndroidAutoSystemHelper1.xml)
     and BusService.hpp runtime identity constants (bus name, object path,
     interface) match the contract tables exactly.
  4. AF_UNIX video and input framing constants, codec field offsets/sizes on
     both the encode and decode paths (VideoCodec.cpp, InputCodec.cpp), and
     socket paths in include/aa/ipc/{VideoSocket,InputSocket}.hpp and
     src/ipc/{video/VideoChannel,input/InputChannel}.cpp match the contract.
  5. Packaging systemd unit effective directives (aa-system-helper.service,
     including the effective UnsetEnvironment= list), D-Bus XML policy
     semantics as they take effect (org.custom.AndroidAutoSystemHelper.conf,
     document order, last matching rule wins), AP config example, and udev
     rules exist and agree with the contract.
  6. Every in-tree repo file path referenced in backticks in
     docs/INTEGRATION-CONTRACT.md exists.

Exits 0 on exact agreement, 1 on any drift (with named INTEGRATION_CONTRACT_*
diagnostics), 2 on usage/unreadable-file error.
"""
from __future__ import annotations

import argparse
import re
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path
from typing import Final

CONTRACT_REL: Final[str] = "docs/INTEGRATION-CONTRACT.md"
RECEIVER_XML_REL: Final[str] = "docs/dbus/org.custom.AndroidAutoReceiver1.xml"
HELPER_XML_REL: Final[str] = "docs/dbus/org.custom.AndroidAutoSystemHelper1.xml"
CONTRACT_HPP_REL: Final[str] = "include/aa/ipc/Contract.hpp"
BUS_WIRE_HPP_REL: Final[str] = "src/ipc/dbus_adapter/BusWire.hpp"
DISPATCH_REPLY_HPP_REL: Final[str] = "src/ipc/dbus_adapter/DispatchReply.hpp"
HELPER_BUS_HPP_REL: Final[str] = "include/aa/helper/BusService.hpp"
VIDEO_SOCKET_HPP_REL: Final[str] = "include/aa/ipc/VideoSocket.hpp"
VIDEO_CODEC_CPP_REL: Final[str] = "src/ipc/video/VideoCodec.cpp"
VIDEO_CHANNEL_CPP_REL: Final[str] = "src/ipc/video/VideoChannel.cpp"
INPUT_SOCKET_HPP_REL: Final[str] = "include/aa/ipc/InputSocket.hpp"
INPUT_CODEC_CPP_REL: Final[str] = "src/ipc/input/InputCodec.cpp"
INPUT_CHANNEL_CPP_REL: Final[str] = "src/ipc/input/InputChannel.cpp"
RECEIVER_SERVICE_REL: Final[str] = "packaging/systemd/aa-receiver.service"
HELPER_SERVICE_REL: Final[str] = "packaging/systemd/aa-system-helper.service"
HELPER_DBUS_CONF_REL: Final[str] = "packaging/dbus/org.custom.AndroidAutoSystemHelper.conf"
AP_CONF_REL: Final[str] = "packaging/ap.conf.example"
UDEV_RULES_REL: Final[str] = "tools/usb-policy/70-android-auto-receiver.rules"

REQUIRED_SECTIONS: Final[tuple[str, ...]] = (
    "1. Ownership and Scope Boundaries",
    "2. Session D-Bus Control Contract",
    "3. AF_UNIX Video and Input Framing ABI and Display Geometry",
    "4. Consumer-Helper Usage (`libaareceiver-consumer`)",
    "5. Autostart Ownership",
    "6. Privileged System Helper Replacement API",
    "7. Audio Streams and Focus Requests",
    "8. HFP Precondition",
    "9. Privacy Rules",
    "10. Infotainment Handoff Checklist",
)

REQUIRED_CHECKLIST_SUBSECTIONS: Final[tuple[str, ...]] = (
    "Settings",
    "App-Host",
    "Startup",
    "Guard Amendments",
)

VIDEO_CONSTANTS: Final[tuple[str, ...]] = (
    "kVideoProtocolVersion",
    "kVideoFrameMagic",
    "kVideoLimitsMagic",
    "kFrameHeaderBytes",
    "kLimitsRecordBytes",
    "kLimitsRecordKind",
    "kMinDatagramPayload",
    "kMaxDatagramPayload",
    "kMaxAccessUnitBytes",
    "kSocketBudgetBytes",
    "kMinSocketBufferBytes",
    "kMaxSocketBufferBytes",
)

INPUT_CONSTANTS: Final[tuple[str, ...]] = (
    "kInputRecordBytes",
    "kInputProtocolVersion",
    "kInputRecordMagic",
    "kInputKindKey",
    "kInputKindTouch",
)

REPO_PATH_PREFIXES: Final[tuple[str, ...]] = (
    "docs/",
    "include/",
    "src/",
    "packaging/",
    "tools/",
    "tests/",
    "cmake/",
)

BACKTICK_RE: Final[re.Pattern[str]] = re.compile(r"`([^`\n]+)`")
SEMVER_RE: Final[re.Pattern[str]] = re.compile(r"^\d+\.\d+\.\d+$")
PUT_CALL_RE: Final[re.Pattern[str]] = re.compile(
    r"\bput\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*(\d+)\s*,\s*([^,]+?)\s*,\s*(\d+)\s*\)"
)
GET_CALL_RE: Final[re.Pattern[str]] = re.compile(
    r"\bget\s*<\s*(\d+)\s*>\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*,\s*(\d+)\s*\)"
)


@dataclass(frozen=True, slots=True)
class Violation:
    code: str
    detail: str

    def render(self) -> str:
        return f"{self.code} {self.detail}"


def parse_markdown_table(text: str, heading: str) -> list[list[str]]:
    """Return data rows (excluding header and separator) under a markdown heading."""
    lines = text.splitlines()
    start = None
    heading_re = re.compile(rf"^#{{2,4}}\s+{re.escape(heading)}\s*$")
    for idx, line in enumerate(lines):
        if heading_re.match(line):
            start = idx + 1
            break
    if start is None:
        return []
    rows: list[list[str]] = []
    in_table = False
    for line in lines[start:]:
        stripped = line.strip()
        if stripped.startswith("#"):
            break
        if stripped.startswith("|") and stripped.endswith("|"):
            in_table = True
            cells = [cell.strip() for cell in stripped.strip("|").split("|")]
            rows.append(cells)
        elif in_table:
            break
    if len(rows) < 2:
        return []
    return rows[2:]


def unquote_code(cell: str) -> str:
    cell = cell.strip()
    if len(cell) >= 2 and cell.startswith("`") and cell.endswith("`"):
        return cell[1:-1]
    return cell


def parse_cpp_int_expr(expr: str) -> int:
    """Evaluate simple integer constant expressions like '16UL * 1024' or '0x41415631U'."""
    cleaned = re.sub(r"//.*", "", expr).strip()
    cleaned = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", cleaned)
    tokens = re.findall(r"0x[0-9a-fA-F]+|\d+|\*", cleaned)
    if not tokens or "".join(tokens) != re.sub(r"\s+", "", cleaned):
        raise ValueError(f"unsupported integer expression: {expr!r}")
    value = int(tokens[0], 0)
    idx = 1
    while idx < len(tokens):
        if tokens[idx] != "*" or idx + 1 >= len(tokens):
            raise ValueError(f"unsupported integer expression: {expr!r}")
        value *= int(tokens[idx + 1], 0)
        idx += 2
    return value


def extract_cpp_int_constant(text: str, name: str) -> int | None:
    pattern = re.compile(rf"\b{re.escape(name)}\s*=\s*([^;]+);")
    match = pattern.search(text)
    if not match:
        return None
    return parse_cpp_int_expr(match.group(1))


def extract_cpp_string_constant(text: str, name: str) -> str | None:
    pattern = re.compile(rf'\b{re.escape(name)}\s*=\s*"([^"]+)"\s*;')
    match = pattern.search(text)
    return match.group(1) if match else None


def extract_cpp_function_body(text: str, func_name: str) -> str | None:
    """Extract the brace-delimited body of a C++ function definition."""
    match = re.search(rf"\b{re.escape(func_name)}\s*\([^)]*\)[^{{;]*\{{", text, re.DOTALL)
    if not match:
        return None
    start = match.end()
    depth = 1
    idx = start
    while idx < len(text) and depth > 0:
        ch = text[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        idx += 1
    return text[start : idx - 1] if depth == 0 else None


def extract_put_layout(body: str, field_classifier: dict[str, str]) -> dict[str, tuple[int, int]]:
    """Map logical field names to (offset, size) from put(buf, offset, expr, size) calls."""
    layout: dict[str, tuple[int, int]] = {}
    reserved_idx = 0
    for match in PUT_CALL_RE.finditer(body):
        offset = int(match.group(1))
        expr = match.group(2).strip()
        size = int(match.group(3))
        field_name: str | None = None
        if expr == "0":
            field_name = f"reserved_{reserved_idx}"
            reserved_idx += 1
        else:
            for token, logical in field_classifier.items():
                if token in expr:
                    field_name = logical
                    break
        if field_name is not None:
            layout[field_name] = (offset, size)
    return layout


def extract_get_layout(body: str) -> set[tuple[int, int]]:
    """Collect every (offset, size) the decoder reads via get<size>(buf, offset).

    The consumer decodes what the producer encodes, so the same field layout must
    be checked on both paths: a one-sided offset change (e.g. the video
    timestamp) corrupts every decoded timestamp while the contract still passes.
    """
    return {(int(match.group(2)), int(match.group(1))) for match in GET_CALL_RE.finditer(body)}


def parse_doc_layout_table(doc: str, heading: str) -> dict[str, tuple[int, int]]:
    layout: dict[str, tuple[int, int]] = {}
    for row in parse_markdown_table(doc, heading):
        if len(row) < 3:
            continue
        field = unquote_code(row[0])
        try:
            offset = int(unquote_code(row[1]), 0)
            size = int(unquote_code(row[2]), 0)
        except ValueError:
            continue
        layout[field] = (offset, size)
    return layout


def effective_unset_environment(values: list[str]) -> list[str]:
    """Resolve UnsetEnvironment= as systemd does: an empty assignment resets the list.

    Values accumulate in file order and each value may name several variables.
    A bare `UnsetEnvironment=` therefore clears everything named before it, so
    checking for a membership anywhere in the raw list would accept a unit that
    re-enables G_DBUS_DEBUG logging (AP PSK leak, #89 L9).
    """
    effective: list[str] = []
    for value in values:
        names = value.split()
        if not names:
            effective = []
            continue
        effective.extend(names)
    return effective


def parse_systemd_unit(text: str) -> dict[str, dict[str, list[str]]]:
    """Parse a systemd unit file into section -> directive -> [values]."""
    sections: dict[str, dict[str, list[str]]] = {}
    current: str | None = None
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith(("#", ";")):
            continue
        if line.startswith("[") and line.endswith("]"):
            current = line[1:-1].strip()
            sections.setdefault(current, {})
            continue
        if current is None or "=" not in line:
            continue
        key, value = line.split("=", 1)
        sections[current].setdefault(key.strip(), []).append(value.strip())
    return sections


def parse_dbus_xml(path: Path) -> tuple[str, str, str, dict[str, tuple[str, str]], dict[str, str], dict[str, tuple[str, str]]]:
    root = ET.fromstring(path.read_text(encoding="utf-8"))
    obj_path = root.get("name", "")
    iface = root.find("interface")
    if iface is None:
        raise ValueError(f"missing <interface> in {path}")
    iface_name = iface.get("name", "")
    semver = ""
    for ann in iface.findall("annotation"):
        if ann.get("name", "").endswith("SemVer"):
            semver = ann.get("value", "")
            break
    methods: dict[str, tuple[str, str]] = {}
    for method in iface.findall("method"):
        in_sig: list[str] = []
        out_sig: list[str] = []
        for arg in method.findall("arg"):
            direction = arg.get("direction", "in")
            (in_sig if direction == "in" else out_sig).append(arg.get("type", ""))
        methods[method.get("name", "")] = ("".join(in_sig), "".join(out_sig))
    signals: dict[str, str] = {}
    for signal in iface.findall("signal"):
        sig = "".join(arg.get("type", "") for arg in signal.findall("arg"))
        signals[signal.get("name", "")] = sig
    properties: dict[str, tuple[str, str]] = {}
    for prop in iface.findall("property"):
        properties[prop.get("name", "")] = (prop.get("type", ""), prop.get("access", ""))
    return obj_path, iface_name, semver, methods, signals, properties


SUPPORTED_POLICY_RULES: Final[frozenset[str]] = frozenset({"own", "send_destination"})


def check_dbus_policy_xml(conf_path: Path, bus_name: str) -> list[Violation]:
    """Validate the helper D-Bus policy as it takes effect, not as it reads.

    dbus-daemon applies policy rules in document order and a later matching rule
    overrides an earlier one, so "a root allow exists" is not the effective
    answer: a later `<deny own=.../>` in the same policy would win. The model
    below tracks the last matching rule per (context, attribute) and requires the
    effective result to be default-deny both for owning and for referencing the
    name, plus a root-only own allow.

    Only the exact supported shape is accepted; anything else fails closed rather
    than being silently ignored (this checker is a drift gate, not a D-Bus
    policy engine). Unsupported: a `<policy>` other than `context="default"` or
    `user="root"`, both context and user on one element, rules whose attributes
    are not exactly one of `own`/`send_destination`, and any child element that
    is not such an allow/deny rule.
    """
    violations: list[Violation] = []

    def drift(detail: str) -> None:
        violations.append(Violation("INTEGRATION_CONTRACT_DBUS_POLICY_DRIFT", detail))

    root = ET.fromstring(conf_path.read_text(encoding="utf-8"))
    if root.tag != "busconfig":
        return [Violation("INTEGRATION_CONTRACT_DBUS_POLICY_DRIFT", f"{conf_path.name} root tag is not <busconfig>")]

    # Only <policy> children are modelled. dbus-daemon also honours <include>,
    # <includedir>, <limit> and other top-level directives at their position,
    # which can add rules after ours; anything other than <policy> (and the
    # standard DOCTYPE-free comments) fails closed.
    for child in root:
        if child.tag != "policy":
            violations.append(Violation(
                "INTEGRATION_CONTRACT_DBUS_POLICY_DRIFT",
                f"unsupported top-level <{child.tag}> in {conf_path.name}; only <policy> is modelled"))
    # Effective rule per (context, attribute); document order, last match wins.
    effective: dict[tuple[str, str], str] = {}
    for policy in root.findall("policy"):
        context = policy.get("context")
        user = policy.get("user")
        if context is not None and user is not None:
            drift(f"policy sets both context and user ({policy.attrib})")
            continue
        if context == "default":
            scope = "default"
        elif user == "root":
            scope = "root"
        else:
            drift(f"unsupported policy scope ({policy.attrib}); expected context=default or user=root")
            continue
        for child in policy:
            if child.tag not in ("allow", "deny"):
                drift(f"unsupported rule <{child.tag}> in {scope} policy")
                continue
            if set(child.attrib) - SUPPORTED_POLICY_RULES:
                drift(f"unsupported rule attributes in {scope} policy ({child.attrib})")
                continue
            if len(child.attrib) != 1:
                drift(f"rule must name exactly one of own/send_destination ({child.attrib})")
                continue
            attribute = next(iter(child.attrib))
            if "*" in child.get(attribute, ""):
                # dbus-daemon treats own="*" (and send_destination="*") as matching
                # every name, so a wildcard rule can supersede the named rules.
                # It is outside the modelled shape: fail closed.
                drift(f"unsupported wildcard rule in {scope} policy ({child.attrib})")
                continue
            if child.get(attribute) != bus_name:
                # Rules for other names do not affect this contract; ignore them.
                continue
            effective[(scope, attribute)] = child.tag

    for scope, attribute, expected, label in (
        ("default", "own", "deny", "default <deny own=...>"),
        ("default", "send_destination", "deny", "default <deny send_destination=...>"),
        ("root", "own", "allow", "root <allow own=...>"),
    ):
        got = effective.get((scope, attribute))
        if got != expected:
            drift(f"effective {label} for {bus_name!r} is {got!r} (a later matching rule wins)")
    return violations



def check_contract(root: Path) -> list[Violation]:
    violations: list[Violation] = []

    def fail(code: str, detail: str) -> None:
        violations.append(Violation(code, detail))

    contract_path = root / CONTRACT_REL
    if not contract_path.is_file():
        return [Violation("INTEGRATION_CONTRACT_MISSING", CONTRACT_REL)]
    doc = contract_path.read_text(encoding="utf-8")

    # 1. Version header and required sections
    version_match = re.search(r"^\-\s+\*\*Contract version:\*\*\s+`([^`]+)`", doc, re.MULTILINE)
    if not version_match or not SEMVER_RE.match(version_match.group(1)):
        fail("INTEGRATION_CONTRACT_VERSION", "missing or invalid SemVer '**Contract version:** `X.Y.Z`'")

    for section in REQUIRED_SECTIONS:
        if not re.search(rf"^##\s+{re.escape(section)}\s*$", doc, re.MULTILINE):
            fail("INTEGRATION_CONTRACT_MISSING_SECTION", section)
    for sub in REQUIRED_CHECKLIST_SUBSECTIONS:
        if not re.search(rf"^###\s+{re.escape(sub)}\s*$", doc, re.MULTILINE):
            fail("INTEGRATION_CONTRACT_MISSING_CHECKLIST", sub)

    # 2. Receiver D-Bus contract (XML + C++ runtime identity)
    rx_xml = root / RECEIVER_XML_REL
    contract_hpp = root / CONTRACT_HPP_REL
    bus_wire_hpp = root / BUS_WIRE_HPP_REL
    dispatch_hpp = root / DISPATCH_REPLY_HPP_REL
    if not rx_xml.is_file():
        fail("INTEGRATION_CONTRACT_MISSING_SOURCE", RECEIVER_XML_REL)
    else:
        obj_path, iface_name, semver, xml_methods, xml_signals, xml_props = parse_dbus_xml(rx_xml)
        rx_meta = {
            row[0]: unquote_code(row[1])
            for row in parse_markdown_table(doc, "2. Session D-Bus Control Contract")
            if len(row) >= 2
        }
        if contract_hpp.is_file():
            chpp = contract_hpp.read_text(encoding="utf-8")
            bus_name = extract_cpp_string_constant(chpp, "kContractBusName")
            cpp_obj_path = extract_cpp_string_constant(chpp, "kContractObjectPath")
            cpp_iface = extract_cpp_string_constant(chpp, "kContractInterface")
            cpp_major = extract_cpp_int_constant(chpp, "kContractMajor")
            if rx_meta.get("Service name") != bus_name:
                fail("INTEGRATION_CONTRACT_RECEIVER_BUS_NAME",
                     f"doc={rx_meta.get('Service name')!r} code={bus_name!r}")
            if cpp_obj_path != obj_path:
                fail("INTEGRATION_CONTRACT_RECEIVER_PATH",
                     f"code={cpp_obj_path!r} xml={obj_path!r}")
            if cpp_iface != iface_name:
                fail("INTEGRATION_CONTRACT_RECEIVER_INTERFACE",
                     f"code={cpp_iface!r} xml={iface_name!r}")
            xml_major = int(semver.split(".")[0]) if SEMVER_RE.match(semver) else None
            if cpp_major is None or cpp_major != xml_major or not iface_name.endswith(str(cpp_major)):
                fail("INTEGRATION_CONTRACT_RECEIVER_MAJOR",
                     f"code={cpp_major!r} xml_semver={semver!r} iface={iface_name!r}")
        else:
            fail("INTEGRATION_CONTRACT_MISSING_SOURCE", CONTRACT_HPP_REL)

        if rx_meta.get("Object path") != obj_path:
            fail("INTEGRATION_CONTRACT_RECEIVER_PATH", f"doc={rx_meta.get('Object path')!r} xml={obj_path!r}")
        if rx_meta.get("Interface") != iface_name:
            fail("INTEGRATION_CONTRACT_RECEIVER_INTERFACE", f"doc={rx_meta.get('Interface')!r} xml={iface_name!r}")
        if rx_meta.get("Contract SemVer") != semver:
            fail("INTEGRATION_CONTRACT_RECEIVER_SEMVER", f"doc={rx_meta.get('Contract SemVer')!r} xml={semver!r}")

        if bus_wire_hpp.is_file():
            max_msg = extract_cpp_int_constant(bus_wire_hpp.read_text(encoding="utf-8"), "kMaxMessageBytes")
            doc_max = rx_meta.get("Max message bytes", "")
            m_num = re.search(r"`(\d+)`", doc_max)
            if not m_num or int(m_num.group(1)) != max_msg:
                fail("INTEGRATION_CONTRACT_DBUS_MAX_BYTES", f"doc={doc_max!r} code={max_msg!r}")
        else:
            fail("INTEGRATION_CONTRACT_MISSING_SOURCE", BUS_WIRE_HPP_REL)

        doc_methods = {
            unquote_code(row[0]): (unquote_code(row[1]), unquote_code(row[2]))
            for row in parse_markdown_table(doc, "Receiver Methods")
            if len(row) >= 3
        }
        for name, sig in xml_methods.items():
            if doc_methods.get(name) != sig:
                fail("INTEGRATION_CONTRACT_RECEIVER_METHOD_DRIFT",
                     f"{name}: doc={doc_methods.get(name)!r} xml={sig!r}")
        for name in sorted(set(doc_methods) - set(xml_methods)):
            fail("INTEGRATION_CONTRACT_RECEIVER_EXTRA_METHOD", name)

        doc_signals = {
            unquote_code(row[0]): unquote_code(row[1])
            for row in parse_markdown_table(doc, "Receiver Signals")
            if len(row) >= 2
        }
        for name, sig in xml_signals.items():
            if doc_signals.get(name) != sig:
                fail("INTEGRATION_CONTRACT_RECEIVER_SIGNAL_DRIFT",
                     f"{name}: doc={doc_signals.get(name)!r} xml={sig!r}")
        for name in sorted(set(doc_signals) - set(xml_signals)):
            fail("INTEGRATION_CONTRACT_RECEIVER_EXTRA_SIGNAL", name)

        doc_props = {
            unquote_code(row[0]): (unquote_code(row[1]), unquote_code(row[2]))
            for row in parse_markdown_table(doc, "Receiver Properties")
            if len(row) >= 3
        }
        for name, spec in xml_props.items():
            if doc_props.get(name) != spec:
                fail("INTEGRATION_CONTRACT_RECEIVER_PROPERTY_DRIFT",
                     f"{name}: doc={doc_props.get(name)!r} xml={spec!r}")
        for name in sorted(set(doc_props) - set(xml_props)):
            fail("INTEGRATION_CONTRACT_RECEIVER_EXTRA_PROPERTY", name)

    if dispatch_hpp.is_file():
        code_errors = set(re.findall(r'"(org\.custom\.AndroidAutoReceiver1\.Error\.[A-Za-z0-9_]+)"',
                                     dispatch_hpp.read_text(encoding="utf-8")))
        doc_errors = {
            unquote_code(row[0])
            for row in parse_markdown_table(doc, "Receiver Errors")
            if len(row) >= 1
        }
        for err in sorted(code_errors - doc_errors):
            fail("INTEGRATION_CONTRACT_MISSING_ERROR", err)
        for err in sorted(doc_errors - code_errors):
            fail("INTEGRATION_CONTRACT_EXTRA_ERROR", err)
    else:
        fail("INTEGRATION_CONTRACT_MISSING_SOURCE", DISPATCH_REPLY_HPP_REL)

    # 3. System Helper D-Bus contract (XML + C++ runtime identity)
    helper_xml = root / HELPER_XML_REL
    helper_bus_hpp = root / HELPER_BUS_HPP_REL
    helper_bus_name = "org.custom.AndroidAutoSystemHelper"
    if not helper_xml.is_file():
        fail("INTEGRATION_CONTRACT_MISSING_SOURCE", HELPER_XML_REL)
    else:
        h_path, h_iface, h_semver, h_methods, _, _ = parse_dbus_xml(helper_xml)
        h_meta = {
            row[0]: unquote_code(row[1])
            for row in parse_markdown_table(doc, "6. Privileged System Helper Replacement API")
            if len(row) >= 2
        }
        if helper_bus_hpp.is_file():
            hb = helper_bus_hpp.read_text(encoding="utf-8")
            h_bus_name = extract_cpp_string_constant(hb, "kBusName")
            h_cpp_path = extract_cpp_string_constant(hb, "kObjectPath")
            h_cpp_iface = extract_cpp_string_constant(hb, "kInterface")
            if h_bus_name:
                helper_bus_name = h_bus_name
            if h_meta.get("Service name") != h_bus_name:
                fail("INTEGRATION_CONTRACT_HELPER_BUS_NAME",
                     f"doc={h_meta.get('Service name')!r} code={h_bus_name!r}")
            if h_cpp_path != h_path:
                fail("INTEGRATION_CONTRACT_HELPER_PATH", f"code={h_cpp_path!r} xml={h_path!r}")
            if h_cpp_iface != h_iface:
                fail("INTEGRATION_CONTRACT_HELPER_INTERFACE", f"code={h_cpp_iface!r} xml={h_iface!r}")
        else:
            fail("INTEGRATION_CONTRACT_MISSING_SOURCE", HELPER_BUS_HPP_REL)

        if h_meta.get("Object path") != h_path:
            fail("INTEGRATION_CONTRACT_HELPER_PATH", f"doc={h_meta.get('Object path')!r} xml={h_path!r}")
        if h_meta.get("Interface") != h_iface:
            fail("INTEGRATION_CONTRACT_HELPER_INTERFACE", f"doc={h_meta.get('Interface')!r} xml={h_iface!r}")
        if h_meta.get("Contract SemVer") != h_semver:
            fail("INTEGRATION_CONTRACT_HELPER_SEMVER", f"doc={h_meta.get('Contract SemVer')!r} xml={h_semver!r}")

        doc_h_methods = {
            unquote_code(row[0]): (unquote_code(row[1]), unquote_code(row[2]))
            for row in parse_markdown_table(doc, "Helper Methods")
            if len(row) >= 3
        }
        for name, sig in h_methods.items():
            if doc_h_methods.get(name) != sig:
                fail("INTEGRATION_CONTRACT_HELPER_METHOD_DRIFT",
                     f"{name}: doc={doc_h_methods.get(name)!r} xml={sig!r}")
        for name in sorted(set(doc_h_methods) - set(h_methods)):
            fail("INTEGRATION_CONTRACT_HELPER_EXTRA_METHOD", name)

    # 4. Video and Input framing constants, codec field layouts, and socket paths
    video_hpp = root / VIDEO_SOCKET_HPP_REL
    if video_hpp.is_file():
        vh_text = video_hpp.read_text(encoding="utf-8")
        doc_v_consts = {
            unquote_code(row[0]): unquote_code(row[1])
            for row in parse_markdown_table(doc, "Video Framing Constants")
            if len(row) >= 2
        }
        for cname in VIDEO_CONSTANTS:
            expected = extract_cpp_int_constant(vh_text, cname)
            got_raw = doc_v_consts.get(cname)
            try:
                got = int(got_raw, 0) if got_raw is not None else None
            except ValueError:
                got = None
            if expected is None or got != expected:
                fail("INTEGRATION_CONTRACT_VIDEO_CONSTANT_DRIFT",
                     f"{cname}: doc={got_raw!r} code={expected!r}")
        for extra in sorted(set(doc_v_consts) - set(VIDEO_CONSTANTS)):
            fail("INTEGRATION_CONTRACT_VIDEO_EXTRA_CONSTANT", extra)
    else:
        fail("INTEGRATION_CONTRACT_MISSING_SOURCE", VIDEO_SOCKET_HPP_REL)

    video_codec = root / VIDEO_CODEC_CPP_REL
    if video_codec.is_file():
        vc_text = video_codec.read_text(encoding="utf-8")
        limits_body = extract_cpp_function_body(vc_text, "encode_limits_record")
        frame_body = extract_cpp_function_body(vc_text, "encode_frame_header")
        if limits_body is None or frame_body is None:
            fail("INTEGRATION_CONTRACT_VIDEO_LAYOUT_DRIFT", "missing encode_limits_record or encode_frame_header")
        else:
            code_limits = extract_put_layout(limits_body, {
                "kVideoLimitsMagic": "magic",
                "kVideoProtocolVersion": "version",
                "kLimitsRecordBytes": "record_size",
                "kLimitsRecordKind": "kind",
                "limits.max_datagram_payload": "max_datagram_payload",
                "limits.max_access_unit_bytes": "max_access_unit_bytes",
                "limits.socket_budget_bytes": "socket_budget_bytes",
            })
            doc_limits = parse_doc_layout_table(doc, "Video Limits Record Layout")
            if code_limits != doc_limits:
                fail("INTEGRATION_CONTRACT_VIDEO_LAYOUT_DRIFT",
                     f"limits layout mismatch: doc={doc_limits!r} code={code_limits!r}")

            code_frame = extract_put_layout(frame_body, {
                "kVideoFrameMagic": "magic",
                "kVideoProtocolVersion": "version",
                "kFrameHeaderBytes": "header_size",
                "header.idr": "idr",
                "header.frame_bytes": "frame_bytes",
                "header.frame": "frame",
                "header.index": "index",
                "header.count": "count",
                "header.sequence": "sequence",
                "header.timestamp_ns": "timestamp_ns",
                "header.payload_bytes": "payload_bytes",
                "metadata": "metadata",
            })
            doc_frame = parse_doc_layout_table(doc, "Video Fragment Header Layout")
            if code_frame != doc_frame:
                fail("INTEGRATION_CONTRACT_VIDEO_LAYOUT_DRIFT",
                     f"frame header layout mismatch: doc={doc_frame!r} code={code_frame!r}")

            # The consumer side must read the SAME layout it is sent. The
            # encoder check alone accepts a decoder-only offset change (e.g.
            # timestamp_ns 36 -> 35), which corrupts every decoded timestamp.
            decode_body = extract_cpp_function_body(vc_text, "decode_frame_header")
            if decode_body is None:
                fail("INTEGRATION_CONTRACT_VIDEO_LAYOUT_DRIFT",
                     "missing decode_frame_header (decoder reads unchecked)")
            else:
                code_decode = extract_get_layout(decode_body)
                doc_pairs = set(doc_frame.values())
                if code_decode != doc_pairs:
                    fail("INTEGRATION_CONTRACT_VIDEO_LAYOUT_DRIFT",
                         f"decoder frame header layout mismatch: doc={sorted(doc_pairs)!r} "
                         f"code={sorted(code_decode)!r}")
    else:
        fail("INTEGRATION_CONTRACT_MISSING_SOURCE", VIDEO_CODEC_CPP_REL)

    input_hpp = root / INPUT_SOCKET_HPP_REL
    if input_hpp.is_file():
        ih_text = input_hpp.read_text(encoding="utf-8")
        doc_i_consts = {
            unquote_code(row[0]): unquote_code(row[1])
            for row in parse_markdown_table(doc, "Input Framing Constants")
            if len(row) >= 2
        }
        for cname in INPUT_CONSTANTS:
            expected = extract_cpp_int_constant(ih_text, cname)
            got_raw = doc_i_consts.get(cname)
            try:
                got = int(got_raw, 0) if got_raw is not None else None
            except ValueError:
                got = None
            if expected is None or got != expected:
                fail("INTEGRATION_CONTRACT_INPUT_CONSTANT_DRIFT",
                     f"{cname}: doc={got_raw!r} code={expected!r}")
        for extra in sorted(set(doc_i_consts) - set(INPUT_CONSTANTS)):
            fail("INTEGRATION_CONTRACT_INPUT_EXTRA_CONSTANT", extra)
    else:
        fail("INTEGRATION_CONTRACT_MISSING_SOURCE", INPUT_SOCKET_HPP_REL)

    input_codec = root / INPUT_CODEC_CPP_REL
    if input_codec.is_file():
        ic_text = input_codec.read_text(encoding="utf-8")
        input_body = extract_cpp_function_body(ic_text, "encode_input_record")
        if input_body is None:
            fail("INTEGRATION_CONTRACT_INPUT_LAYOUT_DRIFT", "missing encode_input_record")
        else:
            code_input = extract_put_layout(input_body, {
                "kInputRecordMagic": "magic",
                "kInputProtocolVersion": "version",
                "kInputRecordBytes": "record_size",
                "header.kind": "kind",
                "header.a": "a",
                "header.b": "b",
                "header.c": "c",
            })
            doc_input = parse_doc_layout_table(doc, "Input Record Layout")
            if code_input != doc_input:
                fail("INTEGRATION_CONTRACT_INPUT_LAYOUT_DRIFT",
                     f"input record layout mismatch: doc={doc_input!r} code={code_input!r}")
    else:
        fail("INTEGRATION_CONTRACT_MISSING_SOURCE", INPUT_CODEC_CPP_REL)

    for rel, expected_sock in (
        (VIDEO_CHANNEL_CPP_REL, "android-auto-receiver/video.sock"),
        (INPUT_CHANNEL_CPP_REL, "android-auto-receiver/input.sock"),
    ):
        src_path = root / rel
        if not src_path.is_file():
            fail("INTEGRATION_CONTRACT_MISSING_SOURCE", rel)
        else:
            if expected_sock not in src_path.read_text(encoding="utf-8"):
                fail("INTEGRATION_CONTRACT_SOCKET_PATH_DRIFT", f"{rel} missing {expected_sock}")
            if f"`{expected_sock}`" not in doc:
                fail("INTEGRATION_CONTRACT_SOCKET_PATH_UNDOCUMENTED", expected_sock)

    # 5. Packaging units and policy files
    pkg_rows = parse_markdown_table(doc, "Packaging and Policy Units")
    doc_pkg_paths = {unquote_code(row[0]) for row in pkg_rows if len(row) >= 1}
    expected_pkg = {RECEIVER_SERVICE_REL, HELPER_SERVICE_REL, HELPER_DBUS_CONF_REL, AP_CONF_REL, UDEV_RULES_REL}
    for rel in sorted(expected_pkg - doc_pkg_paths):
        fail("INTEGRATION_CONTRACT_MISSING_PACKAGING", rel)
    for rel in sorted(expected_pkg):
        if not (root / rel).is_file():
            fail("INTEGRATION_CONTRACT_MISSING_UNIT_FILE", rel)

    helper_service = root / HELPER_SERVICE_REL
    if helper_service.is_file():
        unit = parse_systemd_unit(helper_service.read_text(encoding="utf-8"))
        svc = unit.get("Service", {})
        unit_sec = unit.get("Unit", {})
        install_sec = unit.get("Install", {})

        # Single-valued security/runtime directives must appear exactly once with the expected value.
        for key, expected_val in (
            ("User", "root"),
            ("Group", "root"),
            ("ExecStart", "/usr/libexec/aa-system-helper"),
            ("StateDirectory", "aa-system-helper"),
            ("LimitCORE", "0"),
            ("NoNewPrivileges", "yes"),
        ):
            got_vals = svc.get(key, [])
            if got_vals != [expected_val]:
                fail("INTEGRATION_CONTRACT_UNIT_DRIFT",
                     f"{HELPER_SERVICE_REL} [Service] {key}={got_vals!r} expected [{expected_val!r}]")

        if "dbus.service" not in unit_sec.get("Requires", []):
            fail("INTEGRATION_CONTRACT_UNIT_DRIFT", f"{HELPER_SERVICE_REL} [Unit] missing Requires=dbus.service")
        if "dbus.service" not in unit_sec.get("After", []):
            fail("INTEGRATION_CONTRACT_UNIT_DRIFT", f"{HELPER_SERVICE_REL} [Unit] missing After=dbus.service")
        if "G_DBUS_DEBUG" not in effective_unset_environment(svc.get("UnsetEnvironment", [])):
            fail("INTEGRATION_CONTRACT_UNIT_DRIFT",
                 f"{HELPER_SERVICE_REL} effective UnsetEnvironment= does not unset G_DBUS_DEBUG")
        if "multi-user.target" not in install_sec.get("WantedBy", []):
            fail("INTEGRATION_CONTRACT_UNIT_DRIFT",
                 f"{HELPER_SERVICE_REL} [Install] missing WantedBy=multi-user.target")
        if "/usr/libexec/aa-system-helper" not in doc:
            fail("INTEGRATION_CONTRACT_UNIT_UNDOCUMENTED", "/usr/libexec/aa-system-helper")

    receiver_service = root / RECEIVER_SERVICE_REL
    if receiver_service.is_file():
        unit = parse_systemd_unit(receiver_service.read_text(encoding="utf-8"))
        receiver_doc = [
            token for line in doc.splitlines()
            if line.startswith("- **Packaged user unit (")
            for token in BACKTICK_RE.findall(line)
        ]
        if "PartOf" in unit.get("Unit", {}):
            fail("INTEGRATION_CONTRACT_UNIT_DRIFT",
                 f"{RECEIVER_SERVICE_REL} [Unit] must omit PartOf=")
        for section, key in (
            ("Unit", "StartLimitIntervalSec"),
            ("Unit", "StartLimitBurst"),
            ("Service", "LimitCORE"),
            ("Service", "Restart"),
            ("Service", "RestartSec"),
            ("Service", "RestartPreventExitStatus"),
            ("Install", "WantedBy"),
        ):
            prefix = f"{key}="
            documented = [token.removeprefix(prefix) for token in receiver_doc if token.startswith(prefix)]
            actual = unit.get(section, {}).get(key, [])
            if len(documented) != 1 or not documented[0] or actual != documented:
                fail("INTEGRATION_CONTRACT_UNIT_DRIFT",
                     f"{RECEIVER_SERVICE_REL} [{section}] {key}: doc={documented!r} unit={actual!r}")

    helper_conf = root / HELPER_DBUS_CONF_REL
    if helper_conf.is_file():
        violations.extend(check_dbus_policy_xml(helper_conf, helper_bus_name))

    # 6. Every backticked in-tree repo file reference in the contract exists
    in_handoff = False
    for line in doc.splitlines():
        if line.startswith("## 10. Infotainment Handoff Checklist"):
            in_handoff = True
        for token in BACKTICK_RE.findall(line):
            if not token.startswith(REPO_PATH_PREFIXES):
                continue
            if any(ch in token for ch in ("*", "<", ">", " ", "{", "}")):
                continue
            # Handoff checklist intentionally references zucch1/infotainment paths
            if in_handoff and "(in `zucch1/infotainment`)" in line:
                continue
            target = root / token
            if not target.exists():
                fail("INTEGRATION_CONTRACT_BROKEN_REF", token)

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description="Check docs/INTEGRATION-CONTRACT.md against source")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    try:
        violations = check_contract(args.root.resolve())
    except (OSError, ET.ParseError, ValueError) as exc:
        print(f"integration-contract ERROR {exc}", file=sys.stderr)
        return 2
    if violations:
        for v in violations:
            print(f"integration-contract FAIL {v.render()}")
        return 1
    print("integration-contract PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
