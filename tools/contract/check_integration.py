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
     Contract.hpp constants, BusWire.hpp max message size, and DispatchReply.hpp
     error names match the contract tables exactly.
  3. System Helper D-Bus XML (docs/dbus/org.custom.AndroidAutoSystemHelper1.xml)
     and BusService.hpp constants match the contract tables exactly.
  4. AF_UNIX video and input framing constants and socket paths in
     include/aa/ipc/{VideoSocket,InputSocket}.hpp and
     src/ipc/{video/VideoChannel,input/InputChannel}.cpp match the contract.
  5. Packaging systemd unit, D-Bus policy, AP config example, and udev rules
     exist and agree with the contract.
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
VIDEO_CHANNEL_CPP_REL: Final[str] = "src/ipc/video/VideoChannel.cpp"
INPUT_SOCKET_HPP_REL: Final[str] = "include/aa/ipc/InputSocket.hpp"
INPUT_CHANNEL_CPP_REL: Final[str] = "src/ipc/input/InputChannel.cpp"
HELPER_SERVICE_REL: Final[str] = "packaging/systemd/aa-system-helper.service"
HELPER_DBUS_CONF_REL: Final[str] = "packaging/dbus/org.custom.AndroidAutoSystemHelper.conf"
AP_CONF_REL: Final[str] = "packaging/ap.conf.example"
UDEV_RULES_REL: Final[str] = "tools/usb-policy/70-android-auto-receiver.rules"

REQUIRED_SECTIONS: Final[tuple[str, ...]] = (
    "1. Ownership and Scope Boundaries",
    "2. Session D-Bus Control Contract",
    "3. AF_UNIX Video and Input Framing ABI and Geometry Negotiation",
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

    # 2. Receiver D-Bus contract
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
            if rx_meta.get("Service name") != bus_name:
                fail("INTEGRATION_CONTRACT_RECEIVER_BUS_NAME",
                     f"doc={rx_meta.get('Service name')!r} code={bus_name!r}")
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

    # 3. System Helper D-Bus contract
    helper_xml = root / HELPER_XML_REL
    helper_bus_hpp = root / HELPER_BUS_HPP_REL
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
            if h_meta.get("Service name") != h_bus_name:
                fail("INTEGRATION_CONTRACT_HELPER_BUS_NAME",
                     f"doc={h_meta.get('Service name')!r} code={h_bus_name!r}")
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

    # 4. Video and Input framing constants and socket paths
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
    expected_pkg = {HELPER_SERVICE_REL, HELPER_DBUS_CONF_REL, AP_CONF_REL, UDEV_RULES_REL}
    for rel in sorted(expected_pkg - doc_pkg_paths):
        fail("INTEGRATION_CONTRACT_MISSING_PACKAGING", rel)
    for rel in sorted(expected_pkg):
        if not (root / rel).is_file():
            fail("INTEGRATION_CONTRACT_MISSING_UNIT_FILE", rel)

    helper_service = root / HELPER_SERVICE_REL
    if helper_service.is_file():
        svc_text = helper_service.read_text(encoding="utf-8")
        for token in (
            "ExecStart=/usr/libexec/aa-system-helper",
            "Requires=dbus.service",
            "After=dbus.service",
            "StateDirectory=aa-system-helper",
            "LimitCORE=0",
            "UnsetEnvironment=G_DBUS_DEBUG",
            "WantedBy=multi-user.target",
        ):
            if token not in svc_text:
                fail("INTEGRATION_CONTRACT_UNIT_DRIFT", f"{HELPER_SERVICE_REL} missing {token}")
        if "/usr/libexec/aa-system-helper" not in doc:
            fail("INTEGRATION_CONTRACT_UNIT_UNDOCUMENTED", "/usr/libexec/aa-system-helper")

    helper_conf = root / HELPER_DBUS_CONF_REL
    if helper_conf.is_file():
        conf_text = helper_conf.read_text(encoding="utf-8")
        if 'own="org.custom.AndroidAutoSystemHelper"' not in conf_text:
            fail("INTEGRATION_CONTRACT_DBUS_POLICY_DRIFT",
                 f"{HELPER_DBUS_CONF_REL} missing own=org.custom.AndroidAutoSystemHelper")

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
