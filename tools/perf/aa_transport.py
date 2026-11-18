#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Transport of a qualification session: wired (USB AOA) or wireless (Wi-Fi AP).

Shared by the drive smoke (tools/wired-qa/aa_drive_smoke.py) and the soak
collector (tools/perf/aa_projection_collect.py). Both judge a session with the
same criteria whatever its transport; this module only says how the session is
set up and what the record must say about its link.

Wired: the phone is on ``--port`` and the wired receiver drives it into AOA.
The workload resets the phone's USB to mtp before and after the run (the
existing #137 flow).

Wireless: ``aa-wireless-receiver`` admits the approved phone through the system
helper's AP and Bluetooth bootstrap; the harness never touches Wi-Fi, Bluetooth
or NetworkManager itself. adb (the moving-map stimulus) may still run over a
USB cable, but the session must not: the phone's USB must be in plain mtp, not
AOA (``accessory``), before and after the run, and no wired receiver may run.
The tooling only *reads* the phone's USB functions, Wi-Fi link and Bluetooth
state over adb (``link_snapshot``) and records them; it never switches USB,
because the session does not depend on it.

A result records ``transport``. A gate asked for one transport refuses a record
of the other, and a record without the field is wired (every record before this
field existed came from the wired receiver).
"""
from __future__ import annotations

import re
from collections.abc import Callable

TRANSPORTS = ("wired", "wireless")
DEFAULT_TRANSPORT = "wired"
# /proc comm names (15 chars) of the receivers; any of them running blocks a run.
RECEIVER_COMMS = ("aa-wired-receiv", "aa-wireless-rec")
RECEIVER_BINARY = {"wired": "aa-wired-receiver", "wireless": "aa-wireless-receiver"}
# USB functions that mean the phone is in Android Open Accessory mode.
AOA_FUNCTIONS = ("accessory", "aoa")


def record_transport(record: dict) -> str:
    """The transport a result was measured on; a record without the field is wired."""
    value = record.get("transport", DEFAULT_TRANSPORT)
    return value if isinstance(value, str) else ""


def usb_functions(config: str | None) -> list[str] | None:
    """``sys.usb.config`` ("mtp,adb") as a list; None when it could not be read."""
    if not isinstance(config, str) or not config.strip():
        return None
    return [part for part in config.strip().split(",") if part]


def usb_in_aoa(functions: list[str] | None) -> bool | None:
    """True if AOA is among the functions, None when they are unknown."""
    if functions is None:
        return None
    return any(name in AOA_FUNCTIONS for name in functions)


def frequency_to_channel(mhz: int | None) -> int | None:
    """IEEE 802.11 channel of a 2.4/5/6 GHz centre frequency; None if unknown."""
    if not isinstance(mhz, int):
        return None
    if mhz == 2484:
        return 14
    if 2412 <= mhz <= 2472:
        return (mhz - 2407) // 5
    if 5160 <= mhz <= 5885:
        return (mhz - 5000) // 5
    if 5955 <= mhz <= 7115:
        return (mhz - 5950) // 5
    return None


_RSSI = re.compile(r"\bRSSI:\s*(-?\d+)")
_FREQUENCY = re.compile(r"\bFrequency:\s*(\d+)\s*MHz")


def parse_wifi_status(text: str | None) -> dict:
    """RSSI and frequency from ``adb shell cmd wifi status``; nothing identifying.

    Only the numbers are kept: SSID, BSSID and addresses are never recorded.
    The format is Android's WifiInfo.toString(); a value it does not show is null.
    """
    text = text or ""
    rssi, frequency = _RSSI.search(text), _FREQUENCY.search(text)
    mhz = int(frequency.group(1)) if frequency else None
    rssi_dbm = int(rssi.group(1)) if rssi else None
    if rssi_dbm is not None and not -127 < rssi_dbm <= 0:
        rssi_dbm = None  # Android reports -127 for "invalid"; anything else is not a reading
    return {"connected": ("is connected" in text.lower()) if text else None,
            "frequency_mhz": mhz, "channel": frequency_to_channel(mhz), "rssi_dbm": rssi_dbm}


def _stdout(result: object) -> str | None:
    if getattr(result, "returncode", 1) != 0:
        return None
    out = getattr(result, "stdout", None)
    return out if isinstance(out, str) else None


def link_snapshot(adb: Callable[..., object]) -> dict:
    """What the phone says about its links, read over adb (read-only).

    ``usb_functions`` (and ``usb_aoa``) from ``getprop sys.usb.config``; Wi-Fi
    frequency, channel and RSSI from ``cmd wifi status``; ``bluetooth_on`` from
    ``settings get global bluetooth_on``. Anything that cannot be read is null.
    """
    functions = usb_functions(_stdout(adb("shell", "getprop", "sys.usb.config")))
    wifi = parse_wifi_status(_stdout(adb("shell", "cmd", "wifi", "status")))
    bluetooth = (_stdout(adb("shell", "settings", "get", "global", "bluetooth_on")) or "").strip()
    return {"usb_functions": functions, "usb_aoa": usb_in_aoa(functions), "wifi": wifi,
            "bluetooth_on": {"1": True, "0": False}.get(bluetooth)}


def wireless_usb_refusal(snapshot: dict) -> str | None:
    """Why a wireless session cannot be qualified with this USB state, or None.

    The USB functions must be known and must not include AOA: a phone in AOA
    may be projecting over the cable, and then the session is not wireless.
    """
    if snapshot.get("usb_aoa") is None:
        return "usb_functions_unknown"
    if snapshot["usb_aoa"]:
        return "usb_in_aoa"
    return None


def adb_link(devices_line: str) -> str:
    """How adb reaches the phone: "usb" (a usb: field), "tcp" (an address) or "unknown"."""
    fields = devices_line.split()
    if any(field.startswith("usb:") for field in fields):
        return "usb"
    if fields and ":" in fields[0]:
        return "tcp"
    return "unknown"
