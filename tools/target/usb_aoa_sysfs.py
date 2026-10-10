# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: streamed to the target by probe-usb-aoa.sh; host tests use fixture roots.
"""Read-only sysfs USB evidence: stable port, descriptors, endpoints and advertised power.

The port string (for example ``1-2.1``) is the kernel's stable physical path and
is the only selector used here. Every value is read from the USB sysfs tree; no
device is opened, no authorization attribute is toggled, and ``bMaxPower`` is the
advertised descriptor budget, never a measured current.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Final

USB_ROOT: Final = Path("/sys/bus/usb/devices")
PORT_PATTERN: Final = re.compile(r"[0-9]+-[0-9]+(?:\.[0-9]+)*")
ENDPOINT_PATTERN: Final = re.compile(r"ep_[0-9a-fA-F]{2}")
_TRANSFER_TYPES: Final = {0: "control", 1: "isochronous", 2: "bulk", 3: "interrupt"}


class SysfsError(OSError):
    """Typed sysfs evidence failure carrying the offending path and reason."""

    def __init__(self, path: str, reason: str) -> None:
        self.path = path
        self.reason = reason
        super().__init__(path, reason)

    def __str__(self) -> str:
        return f"{self.reason}: {self.path}"


def valid_port(port: str) -> bool:
    """True only for a bare kernel port path such as ``1-2.1`` (no traversal)."""
    return bool(PORT_PATTERN.fullmatch(port))


def parse_phone_id(text: str) -> tuple[int, int]:
    """Parse an explicit ``vvvv:pppp`` hexadecimal vendor:product identifier."""
    match = re.fullmatch(r"([0-9a-fA-F]{4}):([0-9a-fA-F]{4})", text)
    if match is None:
        raise SysfsError(text, "invalid-phone-id")
    return int(match.group(1), 16), int(match.group(2), 16)


def _read(path: Path) -> str | None:
    try:
        return path.read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return None


def _hex(text: str | None) -> int | None:
    if text is None or not text:
        return None
    try:
        return int(text, 16)
    except ValueError:
        return None


def _milliamps(text: str | None) -> int | None:
    if text is None:
        return None
    digits = text.strip().removesuffix("mA").strip()
    return int(digits) if digits.isdigit() else None


@dataclass(frozen=True, slots=True)
class EndpointInfo:
    address: str
    direction: str
    transfer_type: str
    max_packet_size: int | None


@dataclass(frozen=True, slots=True)
class PortDevice:
    port: str
    busnum: int | None
    devnum: int | None
    vendor_id: int
    product_id: int
    manufacturer: str
    product: str
    serial: str
    advertised_power_ma: int | None
    device_class: int | None
    endpoints: tuple[EndpointInfo, ...]


@dataclass(frozen=True, slots=True)
class PresenceSample:
    port: str
    vendor_id: int | None
    product_id: int | None
    advertised_power_ma: int | None


class Sysfs:
    """One read-only USB sysfs root; a fixture directory is a valid root in tests."""

    def __init__(self, root: Path = USB_ROOT) -> None:
        self._root = root

    def path(self, port: str) -> Path:
        if not valid_port(port):
            raise SysfsError(port, "invalid-port")
        return self._root / port

    def scan(self, port: str) -> PortDevice | None:
        base = self.path(port)
        vendor_id = _hex(_read(base / "idVendor"))
        product_id = _hex(_read(base / "idProduct"))
        if vendor_id is None or product_id is None:
            return None
        return PortDevice(
            port=port,
            busnum=_decimal(_read(base / "busnum")),
            devnum=_decimal(_read(base / "devnum")),
            vendor_id=vendor_id,
            product_id=product_id,
            manufacturer=_read(base / "manufacturer") or "",
            product=_read(base / "product") or "",
            serial=_read(base / "serial") or "",
            advertised_power_ma=_milliamps(_read(base / "bMaxPower")),
            device_class=_hex(_read(base / "bDeviceClass")),
            endpoints=self.endpoints(port),
        )

    def endpoints(self, port: str) -> tuple[EndpointInfo, ...]:
        """Read endpoint packet sizes from each interface directory for the port."""
        self.path(port)
        found: list[EndpointInfo] = []
        for interface in sorted(self._root.glob(f"{port}:*")):
            for endpoint in sorted(interface.glob("ep_*")):
                if not ENDPOINT_PATTERN.fullmatch(endpoint.name):
                    continue
                address = _read(endpoint / "bEndpointAddress") or endpoint.name[3:]
                try:
                    numeric = int(address, 16)
                except ValueError:
                    continue
                attributes = _hex(_read(endpoint / "bmAttributes")) or 0
                size = _read(endpoint / "wMaxPacketSize")
                packet = _hex(size) if size else None
                found.append(EndpointInfo(
                    address=f"{numeric:02x}",
                    direction="in" if numeric & 0x80 else "out",
                    transfer_type=_TRANSFER_TYPES.get(attributes & 0x03, "unknown"),
                    max_packet_size=packet,
                ))
        return tuple(found)

    def presence(self, port: str) -> PresenceSample:
        base = self.path(port)
        return PresenceSample(
            port=port,
            vendor_id=_hex(_read(base / "idVendor")),
            product_id=_hex(_read(base / "idProduct")),
            advertised_power_ma=_milliamps(_read(base / "bMaxPower")),
        )


def _decimal(text: str | None) -> int | None:
    return int(text) if text is not None and text.isdigit() else None
