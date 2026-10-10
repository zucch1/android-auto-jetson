#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/dbus/contract_scenarios.py ROOT CASE [CXX]
"""D-Bus contract schema/codegen scenarios (task 21, gate round 1 fixes).

The happy cases run the shipped tool over the real checked contract XML. Every
negative case writes a REAL temporary mutated fixture (malformed XML, unknown
and overflowing major versions, a high-rate video payload method, duplicate
members, invalid argument directions, masked bulk signatures and SemVer
mismatches) and requires the generate step to fail closed with its named
DBUS_CONTRACT_* diagnostics while writing NO output header.

Embedding verification (round 3) compiles a tiny probe against the generated
header with the native compiler (passed from CMake as CXX), executes it, and
byte-compares what the COMPILED kIntrospectionXml actually contains against the
fixture bytes — adversarial raw-string delimiters, quotes, backslashes,
non-ASCII UTF-8 and CRLF input must all round-trip exactly.
"""
from __future__ import annotations

import subprocess
import sys
import tempfile
from collections.abc import Callable
from pathlib import Path
from typing import Final

TOOL: Final[Path] = Path(__file__).resolve().parents[2] / "tools" / "ipc" / "dbus_contract.py"
CONTRACT: Final[str] = "docs/dbus/org.custom.AndroidAutoReceiver1.xml"
PASS_SUMMARY: Final[str] = "dbus-contract PASS interface=org.custom.AndroidAutoReceiver1"
METHOD_NAMES: Final[tuple[str, ...]] = (
    "RegisterConsumer", "UnregisterConsumer", "StartProjection", "StopProjection",
    "RequestAddPhone", "ConfirmPhonePairing", "CancelPhonePairing", "ForgetPhone",
    "GetState", "GetCapabilities", "SetDisplayViewport", "Ping",
)
PROBE_SOURCE: Final[str] = """#include <aa/ipc/dbus_contract.generated.hpp>
#include <cstdio>
int main(int argc, char** argv) {
    if (argc != 2) {
        return 2;
    }
    std::FILE* out = std::fopen(argv[1], "wb");
    if (out == nullptr) {
        return 3;
    }
    const auto written = std::fwrite(aa::ipc::dbus_contract::kIntrospectionXml.data(), 1,
                                     aa::ipc::dbus_contract::kIntrospectionXml.size(), out);
    if (std::fclose(out) != 0) {
        return 4;
    }
    return written == aa::ipc::dbus_contract::kIntrospectionXml.size() ? 0 : 5;
}
"""

CXX: str | None = None


def run_tool(*, mode: str, xml: Path, out: Path | None = None) -> subprocess.CompletedProcess[str]:
    args = [sys.executable, "-B", str(TOOL), mode, "--xml", str(xml)]
    if out is not None:
        args += ["--out", str(out)]
    return subprocess.run(args, capture_output=True, text=True, check=False)


def expect_failure(result: subprocess.CompletedProcess[str], markers: tuple[str, ...],
                    case: str, out: Path | None) -> int:
    if result.returncode != 1:
        print(f"{case}: expected tool exit 1, got {result.returncode}\n{result.stdout}{result.stderr}")
        return 1
    verdict = 0
    for marker in markers:
        if marker not in result.stdout:
            print(f"{case}: expected {marker} in output\n{result.stdout}")
            verdict = 1
    if out is not None and out.exists():
        print(f"{case}: codegen silently accepted invalid XML (header written)")
        verdict = 1
    return verdict


def reject(root: Path, transform: Callable[[str], str], case: str,
           markers: tuple[str, ...]) -> int:
    text = (root / CONTRACT).read_text(encoding="utf-8")
    changed = transform(text)
    if changed == text:
        print(f"{case}: fixture transform did not change the contract")
        return 1
    scratch = Path(tempfile.mkdtemp(prefix="dbus-contract-"))
    fixture = scratch / "mutated.xml"
    fixture.write_text(changed, encoding="utf-8")
    out = scratch / "should_not_exist.generated.hpp"
    result = run_tool(mode="generate", xml=fixture, out=out)
    return expect_failure(result, markers, case, out)


def generated_path(scratch: Path) -> Path:
    return scratch / "gen" / "aa" / "ipc" / "dbus_contract.generated.hpp"


def compile_and_compare(generated: Path, expected_bytes: bytes, case: str) -> int:
    """Compiles the probe against the generated header with the native
    compiler, runs it, and byte-compares the COMPILED embedding value against
    the fixture bytes (independent of any source-text assumptions)."""
    if CXX is None:
        print(f"{case}: native compiler argument (CXX) is required for embedding verification")
        return 1
    scratch = Path(tempfile.mkdtemp(prefix="dbus-contract-probe-"))
    probe = scratch / "probe.cpp"
    probe.write_text(PROBE_SOURCE, encoding="utf-8")
    binary = scratch / "probe"
    build = subprocess.run(
        [CXX, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-I", str(generated.parents[2]),
         str(probe), "-o", str(binary)],
        capture_output=True, text=True, check=False)
    if build.returncode != 0:
        print(f"{case}: probe failed to compile\n{build.stdout}{build.stderr}")
        return 1
    embedded = scratch / "embedded.bin"
    run = subprocess.run([str(binary), str(embedded)], capture_output=True, text=True, check=False)
    if run.returncode != 0:
        print(f"{case}: probe failed to run (exit {run.returncode})\n{run.stdout}{run.stderr}")
        return 1
    actual = embedded.read_bytes()
    if actual != expected_bytes:
        print(f"{case}: compiled embedding differs from the fixture "
              f"({len(actual)} vs {len(expected_bytes)} bytes)")
        return 1
    return 0


def round_trip(root: Path, transform: Callable[[bytes], bytes], case: str) -> int:
    original = (root / CONTRACT).read_bytes()
    changed = transform(original)
    if changed == original:
        print(f"{case}: fixture transform did not change the contract")
        return 1
    scratch = Path(tempfile.mkdtemp(prefix="dbus-contract-"))
    fixture = scratch / "mutated.xml"
    fixture.write_bytes(changed)
    out = generated_path(scratch)
    result = run_tool(mode="generate", xml=fixture, out=out)
    if result.returncode != 0 or not out.is_file():
        print(f"{case}: expected successful generation for adversarial input\n"
              f"{result.stdout}{result.stderr}")
        return 1
    return compile_and_compare(out, changed, case)


def case_real_check(root: Path) -> int:
    result = run_tool(mode="check", xml=root / CONTRACT)
    if result.returncode != 0:
        print(f"real-check: expected exit 0\n{result.stdout}{result.stderr}")
        return 1
    if PASS_SUMMARY not in result.stdout or "methods=12 signals=5 properties=1" not in result.stdout:
        print(f"real-check: missing PASS summary\n{result.stdout}")
        return 1
    return 0


def case_real_generate(root: Path) -> int:
    scratch = Path(tempfile.mkdtemp(prefix="dbus-contract-"))
    out = generated_path(scratch)
    result = run_tool(mode="generate", xml=root / CONTRACT, out=out)
    if result.returncode != 0 or not out.is_file():
        print(f"real-generate: expected generated header\n{result.stdout}{result.stderr}")
        return 1
    generated = out.read_text(encoding="utf-8")
    if 'kInterfaceName = "org.custom.AndroidAutoReceiver1"' not in generated:
        print("real-generate: missing interface metadata")
        return 1
    if 'kContractSemVer = "1.0.0"' not in generated:
        print("real-generate: missing SemVer metadata")
        return 1
    if "kMethodInSignatures" not in generated or "kMethodOutSignatures" not in generated:
        print("real-generate: missing checked signature tables")
        return 1
    for name in METHOD_NAMES:
        if f'"{name}"' not in generated:
            print(f"real-generate: missing method name {name}")
            return 1
    return compile_and_compare(out, (root / CONTRACT).read_bytes(), "real-generate")


def case_neg_malformed_xml(root: Path) -> int:
    scratch = Path(tempfile.mkdtemp(prefix="dbus-contract-"))
    fixture = scratch / "malformed.xml"
    fixture.write_text("<node name=\"/org/custom/AndroidAutoReceiver\"><interface", encoding="utf-8")
    out = scratch / "should_not_exist.generated.hpp"
    result = run_tool(mode="generate", xml=fixture, out=out)
    return expect_failure(result, ("DBUS_CONTRACT_MALFORMED_XML",), "neg-malformed-xml", out)


def case_neg_unsupported_major(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            '<interface name="org.custom.AndroidAutoReceiver1">',
            '<interface name="org.custom.AndroidAutoReceiver2">'),
        "neg-unsupported-major", ("DBUS_CONTRACT_UNSUPPORTED_MAJOR",))


def case_neg_overflow_major(root: Path) -> int:
    for digits in ("4294967297", "18446744073709551617"):
        verdict = reject(
            root,
            lambda text, digits=digits: text.replace(
                '<interface name="org.custom.AndroidAutoReceiver1">',
                f'<interface name="org.custom.AndroidAutoReceiver{digits}">'),
            f"neg-overflow-major[{digits}]", ("DBUS_CONTRACT_UNSUPPORTED_MAJOR",))
        if verdict != 0:
            return verdict
    return 0


def case_neg_high_rate_payload(root: Path) -> int:
    video_method = (
        '    <method name="PushVideoFrame">\n'
        '      <arg name="frame" type="ay" direction="in"/>\n'
        '    </method>\n')
    return reject(
        root,
        lambda text: text.replace("    <signal name=\"PairingRequest\">",
                                  video_method + "\n    <signal name=\"PairingRequest\">"),
        "neg-high-rate-payload", ("DBUS_CONTRACT_HIGH_RATE_PAYLOAD",))


def case_neg_missing_method(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            '    <method name="StartProjection">\n    </method>\n\n', ''),
        "neg-missing-method", ("DBUS_CONTRACT_MISSING_METHOD",))


def case_neg_bad_signature(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            '<arg name="monotonic_ns" type="t" direction="out"/>',
            '<arg name="monotonic_ns" type="x" direction="out"/>'),
        "neg-bad-signature", ("DBUS_CONTRACT_BAD_SIGNATURE",))


def case_neg_semver_mismatch(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            'name="org.custom.AndroidAutoReceiver1.ContractSemVer" value="1.0.0"',
            'name="org.custom.AndroidAutoReceiver1.ContractSemVer" value="2.0.0"'),
        "neg-semver-mismatch", ("DBUS_CONTRACT_SEMVER_MISMATCH",))


def case_neg_unexpected_method(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            "    <signal name=\"PairingRequest\">",
            "    <method name=\"Extra\">\n    </method>\n\n    <signal name=\"PairingRequest\">"),
        "neg-unexpected-method", ("DBUS_CONTRACT_UNEXPECTED_METHOD",))


def case_neg_duplicate_method(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            "    <signal name=\"PairingRequest\">",
            "    <method name=\"StartProjection\">\n    </method>\n\n"
            "    <signal name=\"PairingRequest\">"),
        "neg-duplicate-method", ("DBUS_CONTRACT_DUPLICATE_MEMBER",))


def case_neg_bogus_direction(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            '<method name="StartProjection">\n    </method>',
            '<method name="StartProjection">\n'
            '      <arg name="frame" type="ay" direction="bogus"/>\n'
            '    </method>'),
        "neg-bogus-direction",
        ("DBUS_CONTRACT_BAD_DIRECTION", "DBUS_CONTRACT_HIGH_RATE_PAYLOAD"))


def case_neg_masking_duplicate(root: Path) -> int:
    bulk = ('<method name="StartProjection">\n'
            '      <arg name="frame" type="ay" direction="in"/>\n'
            '    </method>')
    return reject(
        root,
        lambda text: text.replace(
            '<method name="StartProjection">\n    </method>',
            bulk + '\n    <method name="StartProjection">\n    </method>'),
        "neg-masking-duplicate",
        ("DBUS_CONTRACT_DUPLICATE_MEMBER", "DBUS_CONTRACT_HIGH_RATE_PAYLOAD"))


def case_neg_multi_type_method(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            '<arg name="width" type="u" direction="in"/>\n'
            '      <arg name="height" type="u" direction="in"/>\n'
            '      <arg name="dpi" type="u" direction="in"/>',
            '<arg name="all" type="uuu" direction="in"/>'),
        "neg-multi-type-method", ("DBUS_CONTRACT_BAD_SIGNATURE",))


def case_neg_multi_type_signal(root: Path) -> int:
    return reject(
        root,
        lambda text: text.replace(
            '<signal name="PairingRequest">\n      <arg name="request_id" type="t"/>',
            '<signal name="PairingRequest">\n      <arg name="request_id" type="tt"/>'),
        "neg-multi-type-signal", ("DBUS_CONTRACT_BAD_SIGNATURE",))


def case_neg_raw_delimiter_injection(root: Path) -> int:
    # Adversarial-input regression (NOT a rejection expectation): a valid XML
    # comment carrying the old raw-string closing delimiter must survive
    # validation and embed byte-exactly.
    return round_trip(
        root,
        lambda data: data.replace(b'<interface name=',
                                  b'<!-- )aa_dbus_xml" -->\n  <interface name=', 1),
        "neg-raw-delimiter-injection")


def case_embedding_special_bytes(root: Path) -> int:
    comment = ('<!-- "quoted" back\\slash \u00e9\u65e5 )aa_dbus_xml" '
               'R"aa_dbus_xml( tail -->').encode("utf-8")
    return round_trip(
        root,
        lambda data: data.replace(b'<interface name=', comment + b'\n  <interface name=', 1),
        "embedding-special-bytes")


def case_embedding_crlf(root: Path) -> int:
    return round_trip(root, lambda data: data.replace(b"\n", b"\r\n"), "embedding-crlf")


CASES: dict[str, Callable[[Path], int]] = {
    "real-check": case_real_check,
    "real-generate": case_real_generate,
    "neg-malformed-xml": case_neg_malformed_xml,
    "neg-unsupported-major": case_neg_unsupported_major,
    "neg-overflow-major": case_neg_overflow_major,
    "neg-high-rate-payload": case_neg_high_rate_payload,
    "neg-missing-method": case_neg_missing_method,
    "neg-bad-signature": case_neg_bad_signature,
    "neg-semver-mismatch": case_neg_semver_mismatch,
    "neg-unexpected-method": case_neg_unexpected_method,
    "neg-duplicate-method": case_neg_duplicate_method,
    "neg-bogus-direction": case_neg_bogus_direction,
    "neg-masking-duplicate": case_neg_masking_duplicate,
    "neg-multi-type-method": case_neg_multi_type_method,
    "neg-multi-type-signal": case_neg_multi_type_signal,
    "neg-raw-delimiter-injection": case_neg_raw_delimiter_injection,
    "embedding-special-bytes": case_embedding_special_bytes,
    "embedding-crlf": case_embedding_crlf,
}


def main() -> int:
    if len(sys.argv) not in (3, 4) or sys.argv[2] not in CASES:
        print(f"usage: {sys.argv[0]} ROOT {'|'.join(CASES)} [CXX]")
        return 2
    root = Path(sys.argv[1])
    case = sys.argv[2]
    global CXX
    CXX = sys.argv[3] if len(sys.argv) == 4 else None
    verdict = CASES[case](root)
    print(f"{case}: {'PASS' if verdict == 0 else 'FAIL'}")
    return verdict


if __name__ == "__main__":
    raise SystemExit(main())
