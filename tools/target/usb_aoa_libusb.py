# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: streamed to the target by probe-usb-aoa.sh; host tests never import against hardware.
"""Minimal ctypes libusb-1.0 binding, bounded RAII transport and AOA handshake.

Request codes, request types and the protocol-version contract mirror the
checked-in AASDK reference source (third_party/aasdk/src/USB/
AccessoryModeProtocolVersionQuery.cpp, AccessoryModeSendStringQuery.cpp,
AccessoryModeStartQuery.cpp): GET_PROTOCOL=51, SEND_STRING=52, START=53 with
vendor-specific IN 0xC0 / OUT 0x40. Only the four calls this probe needs are
bound; no bulk/interrupt transfer API is exposed, so the probe cannot start a
protocol session by accident. ``handshake`` drives the bounded AOA sequence and
returns the exact per-step lengths observed.
"""
from __future__ import annotations

import ctypes
import ctypes.util
from dataclasses import dataclass
from types import TracebackType
from typing import Final, Protocol, Self

GET_PROTOCOL: Final = 51
SEND_STRING: Final = 52
START: Final = 53
REQUEST_IN: Final = 0xC0
REQUEST_OUT: Final = 0x40
PROTOCOL_LENGTH: Final = 2
CONTROL_TIMEOUT_MS: Final = 2000
REENUMERATE_TIMEOUT_S: Final = 15.0
GOOGLE_VENDOR: Final = 0x18D1
AOAP_IDS: Final = ((GOOGLE_VENDOR, 0x2D00), (GOOGLE_VENDOR, 0x2D01))
SEND_STRINGS: Final = (
    ("manufacturer", 0, "Android"),
    ("model", 1, "Android Auto"),
    ("description", 2, "Android Auto"),
    ("version", 3, "2.0.1"),
    ("uri", 4, "https://f1xstudio.com"),
    ("serial", 5, "HU-AAAAAA001"),
)


@dataclass(frozen=True, slots=True)
class StringStep:
    name: str
    request: int
    value: int
    index: int
    expected_length: int
    observed_length: int
    complete: bool


@dataclass(frozen=True, slots=True)
class Handshake:
    attempted: bool
    protocol_version: int | None
    steps: tuple[StringStep, ...]
    started: bool
    failed_at: str | None
    requests: int

_SUCCESS: Final = 0
_NO_DEVICE: Final = -4
_ERROR_NAMES: Final = {
    0: "success", -1: "io", -2: "invalid-param", -3: "access", -4: "no-device",
    -5: "not-found", -6: "busy", -7: "timeout", -8: "overflow", -9: "pipe",
    -10: "interrupted", -11: "no-mem", -12: "not-supported", -99: "other",
}


class LibusbError(OSError):
    """Typed libusb failure carrying the operation, native code and detail."""

    def __init__(self, operation: str, code: int, detail: str = "") -> None:
        self.operation = operation
        self.code = code
        self.detail = detail or _ERROR_NAMES.get(code, f"unknown-{code}")
        super().__init__(operation, code, self.detail)

    def __str__(self) -> str:
        return f"{self.operation}: {self.detail} ({self.code})"


class UsbTransport(Protocol):
    """The only transport surface the AOA handshake uses; fakes implement this."""

    def control_in(self, request: int, value: int, index: int,
                   length: int, timeout_ms: int) -> bytes: ...

    def control_out(self, request: int, value: int, index: int,
                    payload: bytes, timeout_ms: int) -> int: ...

    def reset(self) -> None: ...

    def close(self) -> None: ...

    def __enter__(self) -> Self: ...

    def __exit__(self, exc_type: type[BaseException] | None,
                 exc: BaseException | None, traceback: TracebackType | None) -> None: ...


def handshake(transport: UsbTransport, timeout_ms: int = CONTROL_TIMEOUT_MS) -> Handshake:
    """GET_PROTOCOL (exactly 2 bytes, >=1), then every SEND_STRING full length, then START."""
    protocol = transport.control_in(GET_PROTOCOL, 0, 0, PROTOCOL_LENGTH, timeout_ms)
    if len(protocol) != PROTOCOL_LENGTH:
        return Handshake(True, None, (), False, "get-protocol-length", 1)
    version = int.from_bytes(protocol, "little")
    if version < 1:
        return Handshake(True, version, (), False, "get-protocol-version", 1)
    steps: list[StringStep] = []
    for name, index, text in SEND_STRINGS:
        payload = text.encode("ascii") + b"\x00"
        observed = transport.control_out(SEND_STRING, 0, index, payload, timeout_ms)
        complete = observed == len(payload)
        steps.append(StringStep(name, SEND_STRING, 0, index, len(payload), observed, complete))
        if not complete:
            return Handshake(True, version, tuple(steps), False, f"send-string-{name}", 1 + len(steps))
    started = transport.control_out(START, 0, 0, b"", timeout_ms) == 0
    return Handshake(True, version, tuple(steps), started,
                     None if started else "start-short", 2 + len(steps))


class _Descriptor(ctypes.Structure):
    _fields_ = [
        ("bLength", ctypes.c_uint8), ("bDescriptorType", ctypes.c_uint8),
        ("bcdUSB", ctypes.c_uint16), ("bDeviceClass", ctypes.c_uint8),
        ("bDeviceSubClass", ctypes.c_uint8), ("bDeviceProtocol", ctypes.c_uint8),
        ("bMaxPacketSize0", ctypes.c_uint8), ("idVendor", ctypes.c_uint16),
        ("idProduct", ctypes.c_uint16), ("bcdDevice", ctypes.c_uint16),
        ("iManufacturer", ctypes.c_uint8), ("iProduct", ctypes.c_uint8),
        ("iSerialNumber", ctypes.c_uint8), ("bNumConfigurations", ctypes.c_uint8),
    ]


def _declare(lib: ctypes.CDLL) -> None:
    """Bind exact libusb-1.0 signatures; a fake library may also be passed."""
    lib.libusb_init.argtypes = [ctypes.c_void_p]
    lib.libusb_init.restype = ctypes.c_int
    lib.libusb_exit.argtypes = [ctypes.c_void_p]
    lib.libusb_exit.restype = None
    lib.libusb_get_device_list.argtypes = [
        ctypes.c_void_p, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))]
    lib.libusb_get_device_list.restype = ctypes.c_ssize_t
    lib.libusb_free_device_list.argtypes = [ctypes.POINTER(ctypes.c_void_p), ctypes.c_int]
    lib.libusb_free_device_list.restype = None
    lib.libusb_get_bus_number.argtypes = [ctypes.c_void_p]
    lib.libusb_get_bus_number.restype = ctypes.c_uint8
    lib.libusb_get_device_address.argtypes = [ctypes.c_void_p]
    lib.libusb_get_device_address.restype = ctypes.c_uint8
    lib.libusb_get_device_descriptor.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Descriptor)]
    lib.libusb_get_device_descriptor.restype = ctypes.c_int
    lib.libusb_open.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    lib.libusb_open.restype = ctypes.c_int
    lib.libusb_close.argtypes = [ctypes.c_void_p]
    lib.libusb_close.restype = None
    lib.libusb_control_transfer.argtypes = [
        ctypes.c_void_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint16,
        ctypes.c_uint16, ctypes.c_void_p, ctypes.c_uint16, ctypes.c_uint]
    lib.libusb_control_transfer.restype = ctypes.c_int
    lib.libusb_reset_device.argtypes = [ctypes.c_void_p]
    lib.libusb_reset_device.restype = ctypes.c_int


class Libusb:
    """Owns libusb_init/exit; opens a device by bus:address with id verification."""

    def __init__(self, library: ctypes.CDLL | None = None) -> None:
        if library is None:
            path = ctypes.util.find_library("usb-1.0") or "libusb-1.0.so.0"
            library = ctypes.CDLL(path)
        self._lib = library
        _declare(self._lib)
        code = self._lib.libusb_init(None)
        if code != _SUCCESS:
            raise LibusbError("libusb_init", code)
        self._closed = False

    def close(self) -> None:
        if not self._closed:
            self._lib.libusb_exit(None)
            self._closed = True

    def __enter__(self) -> Self:
        return self

    def __exit__(self, exc_type: type[BaseException] | None,
                 exc: BaseException | None, traceback: TracebackType | None) -> None:
        self.close()

    def open(self, bus: int, address: int, vendor_id: int, product_id: int) -> LibusbDevice:
        """Open the device at bus:address whose descriptor ids match exactly."""
        listing = ctypes.POINTER(ctypes.c_void_p)()
        count = self._lib.libusb_get_device_list(None, ctypes.byref(listing))
        if count < 0:
            raise LibusbError("libusb_get_device_list", count)
        handle = ctypes.c_void_p()
        found = False
        try:
            for index in range(count):
                device = listing[index]
                if (self._lib.libusb_get_bus_number(device) != bus or
                        self._lib.libusb_get_device_address(device) != address):
                    continue
                descriptor = _Descriptor()
                result = self._lib.libusb_get_device_descriptor(device, ctypes.byref(descriptor))
                if result != _SUCCESS:
                    continue
                if descriptor.idVendor != vendor_id or descriptor.idProduct != product_id:
                    continue
                result = self._lib.libusb_open(device, ctypes.byref(handle))
                if result != _SUCCESS:
                    raise LibusbError("libusb_open", result)
                found = True
                break
        finally:
            self._lib.libusb_free_device_list(listing, 1)
        if not found:
            raise LibusbError("libusb_open", _NO_DEVICE,
                              f"no {vendor_id:04x}:{product_id:04x} at {bus}:{address}")
        return LibusbDevice(self._lib, handle)


class LibusbDevice:
    """RAII device handle: bounded control transfers, targeted reset, close on exit."""

    def __init__(self, library: ctypes.CDLL, handle: ctypes.c_void_p) -> None:
        self._lib = library
        self._handle = handle

    def control_in(self, request: int, value: int, index: int,
                   length: int, timeout_ms: int) -> bytes:
        buffer = ctypes.create_string_buffer(length)
        transferred = self._lib.libusb_control_transfer(
            self._handle, REQUEST_IN, request, value, index, buffer, length, timeout_ms)
        if transferred < 0:
            raise LibusbError(f"control-in-{request}", transferred)
        return buffer.raw[:transferred]

    def control_out(self, request: int, value: int, index: int,
                    payload: bytes, timeout_ms: int) -> int:
        data = ctypes.create_string_buffer(payload, len(payload)) if payload else None
        transferred = self._lib.libusb_control_transfer(
            self._handle, REQUEST_OUT, request, value, index, data, len(payload), timeout_ms)
        if transferred < 0:
            raise LibusbError(f"control-out-{request}", transferred)
        return transferred

    def reset(self) -> None:
        """Targeted software reset of this device's port; never a hub/controller reset."""
        code = self._lib.libusb_reset_device(self._handle)
        if code != _SUCCESS:
            raise LibusbError("libusb_reset_device", code)

    def close(self) -> None:
        if self._handle is not None:
            self._lib.libusb_close(self._handle)
            self._handle = None

    def __enter__(self) -> Self:
        return self

    def __exit__(self, exc_type: type[BaseException] | None,
                 exc: BaseException | None, traceback: TracebackType | None) -> None:
        self.close()
