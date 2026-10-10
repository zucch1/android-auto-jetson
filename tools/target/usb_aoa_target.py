# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: streamed to the target by probe-usb-aoa.sh; never invoked against hardware by host tests.
"""Bounded AOA capability probe: stable port selection, switch, cycles and stability.

Only an explicitly approved phone (its exact vendor:product id at the requested
sysfs port) or a device already in AOA mode (18d1:2d00/2d01) is qualified. Any
other device is reported as an unqualified candidate with zero control requests.
The pinned descriptor strings are the checked-in AASDK reference values.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from collections.abc import Callable, Sequence
from dataclasses import asdict, dataclass, field
from pathlib import Path

import usb_aoa_hostenv as hostenv
import usb_aoa_libusb as libusb
import usb_aoa_sysfs as sysfs
from usb_aoa_hostenv import CommandRunner, GadgetStatus, HostEnvironment, SubprocessRunner
from usb_aoa_libusb import AOAP_IDS, Handshake, Libusb, UsbTransport, handshake
from usb_aoa_sysfs import PortDevice, PresenceSample, Sysfs, valid_port, parse_phone_id

PRESENCE_INTERVAL_S = 1.0
POLL_INTERVAL_S = 0.25


@dataclass(frozen=True, slots=True)
class Selection:
    port: str
    vendor_id: int
    product_id: int
    approved: bool
    already_aoa: bool
    unqualified: bool
    reason: str


@dataclass(frozen=True, slots=True)
class CycleOutcome:
    index: int
    reset_ok: bool
    reenumerated: bool
    vendor_id: int | None
    product_id: int | None
    reestablished_aoa: bool
    detail: str


@dataclass(frozen=True, slots=True)
class Presence:
    duration_s: float
    samples: int
    stable: bool
    advertised_power_ma: int | None
    vendor_id: int | None
    product_id: int | None


@dataclass(frozen=True, slots=True)
class Options:
    target: str
    port: str
    phone_id: str
    phone_ids: tuple[int, int]
    cycles: int
    duration_s: float
    interval_s: float


@dataclass(frozen=True, slots=True)
class ProbeEnvironment:
    sysfs: Sysfs
    transport_factory: Callable[[int, int, int, int], UsbTransport]
    runner: CommandRunner
    udc_root: Path
    configfs_root: Path
    clock: Callable[[], float]
    sleep: Callable[[float], None]


@dataclass(slots=True)  # noqa: MUTABLE_OK - accumulates observations across bounded phases
class Report:
    schema: str = "aa-usb-aoa-probe/1"
    target: str = ""
    phase: str = "read-only"
    port: str = ""
    phone_id: str = ""
    qualified: bool = False
    selection: Selection | None = None
    device: PortDevice | None = None
    aoa: Handshake | None = None
    cycles: list[CycleOutcome] = field(default_factory=list)
    presence: Presence | None = None
    gadget_before: GadgetStatus | None = None
    gadget: GadgetStatus | None = None
    host_environment: HostEnvironment | None = None
    blockers: list[str] = field(default_factory=list)
    raw: dict[str, str] = field(default_factory=dict)
    protected_inventory: dict[str, str] = field(default_factory=dict)


def select(device: PortDevice, phone_ids: tuple[int, int]) -> Selection:
    """Classify one device; only an exact approved id or live AOA mode qualifies."""
    ids = (device.vendor_id, device.product_id)
    if ids in AOAP_IDS:
        return Selection(device.port, *ids, False, True, False, "already-aoa")
    if ids == phone_ids:
        return Selection(device.port, *ids, True, False, False, "approved-phone")
    return Selection(device.port, *ids, False, False, True, "unqualified-candidate")


def open_transport(env: ProbeEnvironment, device: PortDevice) -> UsbTransport:
    return env.transport_factory(device.busnum or 0, device.devnum or 0,
                                 device.vendor_id, device.product_id)


def usable_aoa(device: PortDevice | None) -> bool:
    if device is None or (device.vendor_id, device.product_id) not in AOAP_IDS:
        return False
    directions = {endpoint.direction for endpoint in device.endpoints
                  if endpoint.transfer_type == "bulk" and
                  endpoint.max_packet_size is not None and endpoint.max_packet_size > 0}
    return {"in", "out"} <= directions


def wait_for_port(env: ProbeEnvironment, port: str, expected: tuple[tuple[int, int], ...]) -> PortDevice | None:
    """Poll the SAME sysfs port until it presents one of the expected ids, bounded."""
    deadline = env.clock() + libusb.REENUMERATE_TIMEOUT_S
    while env.clock() < deadline:
        device = env.sysfs.scan(port)
        if device is not None and (device.vendor_id, device.product_id) in expected:
            return device
        env.sleep(POLL_INTERVAL_S)
    return None


def cycle(options: Options, env: ProbeEnvironment, index: int) -> CycleOutcome:
    """Targeted software reset, bounded same-port re-enumeration, AOA re-establishment."""
    device = env.sysfs.scan(options.port)
    if device is None:
        return CycleOutcome(index, False, False, None, None, False, "port-missing")
    if select(device, options.phone_ids).unqualified:
        return CycleOutcome(index, False, False, device.vendor_id, device.product_id,
                            False, "unqualified-candidate")
    try:
        with open_transport(env, device) as transport:
            transport.reset()
    except OSError as error:
        return CycleOutcome(index, False, False, device.vendor_id, device.product_id,
                            False, type(error).__name__)
    observed = wait_for_port(env, options.port, (options.phone_ids, *AOAP_IDS))
    if observed is None:
        return CycleOutcome(index, True, False, None, None, False, "reenumeration-timeout")
    ids = (observed.vendor_id, observed.product_id)
    if ids not in AOAP_IDS:
        try:
            with open_transport(env, observed) as transport:
                result = handshake(transport)
        except OSError as error:
            return CycleOutcome(index, True, True, *ids, False, type(error).__name__)
        if not result.started:
            return CycleOutcome(index, True, True, *ids, False, "aoa-restart-incomplete")
        observed = wait_for_port(env, options.port, AOAP_IDS)
        if observed is None:
            return CycleOutcome(index, True, True, None, None, False, "aoa-reenumeration-timeout")
        ids = (observed.vendor_id, observed.product_id)
    established = usable_aoa(observed)
    return CycleOutcome(index, True, True, *ids, established,
                        "" if established else "aoa-bulk-endpoints-unusable")


def observe(options: Options, env: ProbeEnvironment) -> Presence:
    """Sample port presence and advertised budget only; never a measured current."""
    first = env.sysfs.presence(options.port)
    samples = 1
    stable = (first.vendor_id, first.product_id) in AOAP_IDS
    deadline = env.clock() + options.duration_s
    while env.clock() < deadline:
        env.sleep(min(options.interval_s, deadline - env.clock()))
        sample: PresenceSample = env.sysfs.presence(options.port)
        samples += 1
        if sample.vendor_id is None or (sample.vendor_id, sample.product_id) != (first.vendor_id, first.product_id):
            stable = False
    return Presence(options.duration_s, samples, stable, first.advertised_power_ma,
                    first.vendor_id, first.product_id)


def run(options: Options, env: ProbeEnvironment) -> Report:
    """Qualify and exercise one explicit port; unqualified devices make zero requests."""
    report = Report(target=options.target, port=options.port, phone_id=options.phone_id)
    report.gadget_before = hostenv.gadget_status(env.udc_root, env.configfs_root)
    report.host_environment = hostenv.host_environment(env.runner)
    try:
        device = env.sysfs.scan(options.port)
        report.device = device
        if device is None:
            report.selection = Selection(options.port, 0, 0, False, False, True, "port-empty")
            report.blockers.append("port-empty")
            return report
        selection = select(device, options.phone_ids)
        report.selection = selection
        if selection.unqualified:
            report.blockers.append("unqualified-candidate")
            return report
        if selection.approved:
            report.phase = "switch"
            report.aoa = _attempt_switch(env, device, report)
            if report.aoa is None or not report.aoa.started:
                if report.aoa is not None:
                    report.blockers.append("aoa-switch-incomplete")
                return report
            device = wait_for_port(env, options.port, AOAP_IDS)
            report.device = device
            if device is None:
                report.blockers.append("aoa-reenumeration-timeout")
                return report
            report.phase = "switched"
        else:
            report.phase = "already-aoa"
        if not usable_aoa(device):
            report.blockers.append("aoa-bulk-endpoints-unusable")
            return report
        for index in range(options.cycles):
            outcome = cycle(options, env, index)
            report.cycles.append(outcome)
            if not (outcome.reset_ok and outcome.reenumerated and outcome.reestablished_aoa):
                report.blockers.append(f"cycle-{index}-not-reestablished")
                break
        report.presence = observe(options, env)
        if not report.presence.stable:
            report.blockers.append("usb-presence-unstable")
        report.device = env.sysfs.scan(options.port)
        if not usable_aoa(report.device):
            report.blockers.append("aoa-bulk-endpoints-unusable")
    finally:
        report.gadget = hostenv.gadget_status(env.udc_root, env.configfs_root)
    report.qualified = (not report.blockers and usable_aoa(report.device) and
                        report.presence is not None and report.presence.stable and
                        len(report.cycles) == options.cycles)
    return report


def _attempt_switch(env: ProbeEnvironment, device: PortDevice, report: Report) -> Handshake | None:
    try:
        with open_transport(env, device) as transport:
            return handshake(transport)
    except OSError as error:
        report.blockers.append("aoa-handshake-failed")
        report.raw["aoa_error_type"] = type(error).__name__
        return None


class _LazyLibusb:
    """Opens libusb only when an approved transport is actually requested."""

    def __init__(self) -> None:
        self._library: Libusb | None = None

    def open(self, bus: int, address: int, vendor_id: int, product_id: int) -> UsbTransport:
        if self._library is None:
            self._library = Libusb()
        return self._library.open(bus, address, vendor_id, product_id)

    def close(self) -> None:
        if self._library is not None:
            self._library.close()
            self._library = None


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", default="")
    parser.add_argument("--port", required=True)
    parser.add_argument("--phone-id", required=True)
    parser.add_argument("--cycles", type=int, default=0)
    parser.add_argument("--duration", type=float, default=30.0)
    parser.add_argument("--interval", type=float, default=PRESENCE_INTERVAL_S)
    parser.add_argument("--sysfs-root", type=Path, default=sysfs.USB_ROOT)
    parser.add_argument("--udc-root", type=Path, default=hostenv.UDC_ROOT)
    parser.add_argument("--configfs-root", type=Path, default=hostenv.CONFIGFS_ROOT)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    report = Report(target=args.target, port=args.port, phone_id=args.phone_id)
    try:
        phone_ids = parse_phone_id(args.phone_id)
    except sysfs.SysfsError:
        report.blockers.append("invalid-arguments")
        print(json.dumps(asdict(report), sort_keys=True))
        return 1
    if (not valid_port(args.port) or not 0 <= args.cycles <= 10 or
            not 0.0 <= args.duration <= 120.0 or not 0.05 <= args.interval <= 5.0):
        report.blockers.append("invalid-arguments")
        print(json.dumps(asdict(report), sort_keys=True))
        return 1
    options = Options(args.target, args.port, args.phone_id, phone_ids,
                      args.cycles, args.duration, args.interval)
    pool = _LazyLibusb()
    env = ProbeEnvironment(Sysfs(args.sysfs_root), pool.open, SubprocessRunner(),
                           args.udc_root, args.configfs_root, time.monotonic, time.sleep)
    try:
        report = run(options, env)
    except OSError as error:
        report.blockers.append("probe-operation-failed")
        report.raw["error_type"] = type(error).__name__
    finally:
        pool.close()
    print(json.dumps(asdict(report), sort_keys=True))
    return int(bool(report.blockers) or not report.qualified)


if __name__ == "__main__":
    sys.exit(main())
