# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: tools/target/probe-usb-aoa.sh --help
"""Host-side bounded USB/AOA capability probe; streams the target modules over SSH.

The approved phone is selected by an explicit stable sysfs port and its exact
vendor:product id; supplying both at the CLI is the owner's authorization to
issue the bounded AOA control/switch requests. Any other device is reported as an
unqualified candidate and receives no control request. Stdout is JSON only;
diagnostics go to stderr. No target file is staged: sources stream over stdin.
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path
from typing import Final, TypedDict

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.target_safety.kernel import Blocked
from tools.target_safety.snapshot_diff import diff_manifests

type JsonValue = None | bool | int | float | str | list[JsonValue] | dict[str, JsonValue]

PORT_PATTERN: Final = re.compile(r"[0-9]+-[0-9]+(?:\.[0-9]+)*")
PHONE_PATTERN: Final = re.compile(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{4}")
HOST_PATTERN: Final = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.@-]*")
TARGET_MODULES: Final = ("usb_aoa_libusb", "usb_aoa_sysfs", "usb_aoa_hostenv", "usb_aoa_target")
SSH_OPTIONS: Final = ("BatchMode=yes", "StrictHostKeyChecking=yes",
                      "HostKeyAlgorithms=ssh-ed25519", "ControlMaster=no",
                      "ControlPath=none", "ConnectTimeout=10")
SCHEMA: Final = "aa-usb-aoa-probe/1"
RUN_OVERHEAD_S: Final = 120
CYCLE_BOUND_S: Final = 20
PENDING_INVENTORY: Final = {
    "status": "external-proof-pending",
    "note": "parent records before/after equivalence with the existing snapshot APIs",
}


class ProbeReport(TypedDict):
    schema: str
    target: str
    qualified: bool
    phase: str
    selection: JsonValue
    device: JsonValue
    aoa: JsonValue
    cycles: list[JsonValue]
    presence: JsonValue
    gadget: JsonValue
    host_environment: JsonValue
    blockers: list[str]
    raw: dict[str, str]
    protected_inventory: dict[str, str]


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--jetson", default="jetson.local")
    parser.add_argument("--port", default="")
    parser.add_argument("--phone-id", default="")
    parser.add_argument("--cycles", type=int, default=0)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--interval", type=float, default=1.0)
    parser.add_argument("--source-address", default="")
    parser.add_argument("--address", default="")
    parser.add_argument("--host-key-alias", default="")
    parser.add_argument("--sudo", action="store_true")
    parser.add_argument("--sysfs-root", default="")
    parser.add_argument("--udc-root", default="")
    parser.add_argument("--configfs-root", default="")
    parser.add_argument("--before-manifest", type=Path)
    parser.add_argument("--after-manifest", type=Path)
    return parser


def _ssh_prefix(args: argparse.Namespace) -> list[str]:
    command = ["ssh"]
    for option in SSH_OPTIONS:
        command += ["-o", option]
    if args.source_address:
        command += ["-b", args.source_address]
    if args.host_key_alias:
        command += ["-o", f"HostKeyAlias={args.host_key_alias}"]
    command.append(args.address or args.jetson)
    return command


def _remote_command(args: argparse.Namespace) -> list[str]:
    return (["sudo", "-n", "python3", "-B", "-"] if args.sudo else ["python3", "-B", "-"])


def _target_argv(args: argparse.Namespace) -> list[str]:
    argv = ["--target", args.jetson, "--port", args.port, "--phone-id", args.phone_id,
            "--cycles", str(args.cycles), "--duration", str(args.duration), "--interval", str(args.interval)]
    for flag, value in (("--sysfs-root", args.sysfs_root), ("--udc-root", args.udc_root),
                        ("--configfs-root", args.configfs_root)):
        if value:
            argv += [flag, value]
    return argv


def _payload(here: Path, argv: list[str]) -> str:
    lines = ["import sys, types"]
    for name in TARGET_MODULES:
        lines.append(f"module = types.ModuleType({name!r})")
        lines.append(f"sys.modules[{name!r}] = module")
        lines.append(f"exec({(here / (name + '.py')).read_text()!r}, module.__dict__)")
    lines.append(f"sys.exit(module.main({argv!r}))")
    return "\n".join(lines)


def _diagnose(text: str) -> None:
    if text.strip():
        print(text.rstrip(), file=sys.stderr)


def _inventory(args: argparse.Namespace, report: ProbeReport) -> None:
    """Validate an optional caller-retained manifest pair; default stays proof-pending."""
    if not (args.before_manifest or args.after_manifest):
        report["protected_inventory"] = dict(PENDING_INVENTORY)
        return
    if not (args.before_manifest and args.after_manifest):
        report["blockers"].append("both-manifests-required")
        return
    try:
        artifact = args.before_manifest.with_name(args.before_manifest.stem + "-usb-diff.json")
        verdict = diff_manifests(args.before_manifest, args.after_manifest, artifact)
        report["protected_inventory"] = {"status": "validated", "verdict": verdict.verdict,
                                         "diff": str(artifact)}
        if verdict.exit_status:
            report["blockers"].append("protected-inventory-not-equivalent")
    except (Blocked, OSError, ValueError) as error:
        report["protected_inventory"] = {"status": "validation-failed"}
        report["blockers"].append("protected-inventory-validation-failed")
        report["raw"]["inventory_error_type"] = type(error).__name__


def _base_report(args: argparse.Namespace) -> ProbeReport:
    return {"schema": SCHEMA, "target": args.jetson, "qualified": False, "phase": "not-run",
            "selection": None, "device": None, "aoa": None, "cycles": [], "presence": None,
            "gadget": None, "host_environment": None, "blockers": [], "raw": {},
            "protected_inventory": dict(PENDING_INVENTORY)}


def main(argv: list[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    report = _base_report(args)
    if not (PORT_PATTERN.fullmatch(args.port) and PHONE_PATTERN.fullmatch(args.phone_id)
            and HOST_PATTERN.fullmatch(args.jetson)):
        report["blockers"].append("port-and-phone-id-required")
        print(json.dumps(report, sort_keys=True))
        return 1
    if not (0 <= args.cycles <= 10 and 0.0 <= args.duration <= 120.0
            and 0.05 <= args.interval <= 5.0):
        report["blockers"].append("invalid-arguments")
        print(json.dumps(report, sort_keys=True))
        return 1
    here = Path(__file__).resolve().parent
    prefix = _ssh_prefix(args)
    bound = RUN_OVERHEAD_S + args.cycles * CYCLE_BOUND_S + args.duration
    try:
        preflight = subprocess.run(prefix + ["true"], capture_output=True, text=True,
                                   timeout=30, check=False)
        _diagnose(preflight.stderr)
        if preflight.returncode != 0:
            report["blockers"].append("ssh-preflight-failed")
            print(json.dumps(report, sort_keys=True))
            return 1
        completed = subprocess.run(prefix + _remote_command(args), input=_payload(here, _target_argv(args)),
                                   capture_output=True, text=True, timeout=bound, check=False)
        _diagnose(completed.stderr)
        observed: ProbeReport = json.loads(completed.stdout)
        if observed.get("schema") != SCHEMA or observed.get("target") != args.jetson:
            raise ValueError("invalid-target-report")
        report = observed
        if completed.returncode and not report["blockers"]:
            report["blockers"].append("target-probe-failed")
    except (OSError, subprocess.SubprocessError, ValueError) as error:
        print(f"usb-aoa-probe: {type(error).__name__}", file=sys.stderr)
        report["blockers"].append("ssh-or-target-operation-failed")
        report["raw"]["host_error_type"] = type(error).__name__
    _inventory(args, report)
    print(json.dumps(report, sort_keys=True))
    return 1 if (report["blockers"] or not report["qualified"]) else 0


if __name__ == "__main__":
    sys.exit(main())
