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
helper's AP and Bluetooth bootstrap; the harness never touches Wi-Fi, Bluetooth,
NetworkManager or the helper itself. Android Auto does not start a wireless
session while the phone is attached over USB, so the phone's cable must be
**unplugged**: the Jetson's own USB bus (sysfs, ``host_usb_evidence``) must show
no device with the phone's serial and no Google AOA device (18d1:2d00-2d05)
on any port, before, during and after the run (``usb_attached``).

adb (the moving-map stimulus) then runs over the helper's AP link: the operator
runs ``adb tcpip 5555`` once beforehand while the phone is cabled (runbook);
after the phone joins the AP the harness reads the station addresses from the
AP's DHCP leases and neighbour table on the AP interface (IPv4 addresses only;
MACs, the PSK and the SSID are never read into the record or printed), runs
``adb connect <ip>:5555`` and binds by identity: ``adb -s <ip>:5555
get-serialno`` must equal the expected phone serial (``bind_adb``). A phone
adb cannot reach, or a different phone, BLOCKS the run (``adb_unreachable`` /
``adb_phone_mismatch``). The phone's own link state (USB functions, Wi-Fi
frequency/channel/RSSI, Bluetooth) is then read over that adb into the record.

A result records ``transport``. A gate asked for one transport refuses a record
of the other, and a record without the field is wired (every record before this
field existed came from the wired receiver).
"""
from __future__ import annotations

import ipaddress
import re
import subprocess
from collections.abc import Callable, Iterable
from pathlib import Path

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


# ---- Receiver diagnostics --------------------------------------------------

def diag_kind(event: dict) -> str | None:
    """The diagnostic kind of a receiver line, bare or wrapped; None if it is no diagnostic.

    The wired receiver wraps diagnostics as ``{"event":"diag","kind":...}``; the
    wireless receiver on main prints the serializer's bare object
    ``{"kind":...,"session":N,"mono_ns":...}`` (PR #163 adds the wrapper there).
    """
    kind = event.get("kind")
    if not isinstance(kind, str):
        return None
    if event.get("event") == "diag" or "event" not in event:
        return kind
    return None


# ---- Jetson-side USB evidence (the cable is out) ----------------------------

GOOGLE_VID = "18d1"
AOA_PIDS = frozenset(f"2d0{n}" for n in range(6))  # 18d1:2d00-2d05, every AOA mode
USB_DEVICES = Path("/sys/bus/usb/devices")


def _read(path: Path) -> str | None:
    try:
        return path.read_text().strip()
    except OSError:
        return None


def host_usb_evidence(phone_serial: str, root: Path | None = None) -> dict:
    """Whether the Jetson sees the phone, or any AOA device, on its USB bus.

    Read-only sysfs. ``readable`` is False when the bus cannot be listed; then
    both answers are unknown (None) and the run is BLOCKED (fail closed).
    The serial is compared, never recorded here.
    """
    root = USB_DEVICES if root is None else root
    try:
        devices = [entry for entry in root.iterdir() if (entry / "idVendor").exists()]
    except OSError:
        return {"readable": False, "phone_attached": None, "aoa_attached": None}
    phone = aoa = False
    for device in devices:
        vendor, product = _read(device / "idVendor"), _read(device / "idProduct")
        if vendor == GOOGLE_VID and product in AOA_PIDS:
            aoa = True
        if phone_serial and _read(device / "serial") == phone_serial:
            phone = True
    return {"readable": True, "phone_attached": phone, "aoa_attached": aoa}


def host_usb_refusal(evidence: dict | None) -> str | None:
    """``usb_attached`` (with its sub-reason) or ``usb_state_unreadable``; None when the cable is out."""
    if not isinstance(evidence, dict) or evidence.get("readable") is not True:
        return "usb_state_unreadable"
    if evidence.get("aoa_attached") is not False:
        return "usb_attached:usb_in_aoa"
    if evidence.get("phone_attached") is not False:
        return "usb_attached:phone_on_usb"
    return None


# ---- adb over the helper's AP link ------------------------------------------

AP_CONF = Path("/etc/android-auto-receiver/ap.conf")
NM_LEASES = "/var/lib/NetworkManager/dnsmasq-{interface}.leases"
ADB_TCP_PORT = 5555
_INTERFACE = re.compile(r"[A-Za-z0-9_.:-]{1,15}")


def ap_interface(conf: Path = AP_CONF) -> str | None:
    """The AP interface named in the helper's ap.conf (``v1 IFACE ...``); None if unreadable."""
    fields = (_read(conf) or "").split()
    if len(fields) >= 2 and fields[0] == "v1" and _INTERFACE.fullmatch(fields[1]):
        return fields[1]
    return None


def _ipv4s(tokens: Iterable[str]) -> list[str]:
    out = []
    for token in tokens:
        try:
            address = ipaddress.IPv4Address(token)
        except ValueError:
            continue
        if address.is_private and str(address) not in out:
            out.append(str(address))
    return out


def ap_station_addresses(interface: str, leases_text: str | None, neigh_text: str | None) -> list[str]:
    """IPv4 addresses of the AP's stations: DHCP leases first, then reachable neighbours.

    Lease lines are ``expiry MAC IP host client-id``; only field 3 is kept.
    Neighbour lines are ``IP lladdr MAC STATE``; only the address of a
    non-FAILED entry is kept. MACs are never returned.
    """
    leases = _ipv4s(line.split()[2] for line in (leases_text or "").splitlines()
                    if len(line.split()) >= 3)
    neigh = _ipv4s(line.split()[0] for line in (neigh_text or "").splitlines()
                   if line.split() and line.split()[-1] not in ("FAILED", "INCOMPLETE"))
    return leases + [address for address in neigh if address not in leases]


def read_ap_stations(interface: str | None,
                     run: Callable[..., subprocess.CompletedProcess] = subprocess.run) -> list[str]:
    """Read the lease file and ``ip -4 neigh show dev IFACE`` (read-only)."""
    if not interface:
        return []
    leases = _read(Path(NM_LEASES.format(interface=interface)))  # module attribute: test seam
    try:
        neigh = run(["ip", "-4", "neigh", "show", "dev", interface], capture_output=True,
                    text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        neigh = None
    return ap_station_addresses(interface, leases, neigh)


def bind_adb(expected_serial: str, candidates: list[str],
             adb: Callable[..., object]) -> tuple[str | None, str | None]:
    """Connect adb to the station that is the expected phone: (adb serial, None) or (None, reason).

    ``adb(*args)`` runs one bounded host adb command. Each candidate is tried as
    ``<ip>:5555``; the phone is bound only when ``get-serialno`` over that
    connection equals ``expected_serial``. A candidate that answers with another
    serial is disconnected again. Reasons: ``adb_unreachable`` (no candidate, or
    none answered), ``adb_phone_mismatch`` (only other phones answered).
    """
    other = False
    for address in candidates:
        target = f"{address}:{ADB_TCP_PORT}"
        connected = adb("connect", target)
        if getattr(connected, "returncode", 1) != 0 or "connected" not in (
                getattr(connected, "stdout", "") or "") or "cannot" in (getattr(connected, "stdout", "") or ""):
            continue
        answer = adb("-s", target, "get-serialno")
        serial = (getattr(answer, "stdout", "") or "").strip() if getattr(answer, "returncode", 1) == 0 else ""
        if serial and serial == expected_serial:
            return target, None
        if serial:
            other = True
        adb("disconnect", target)
    return None, "adb_phone_mismatch" if other else "adb_unreachable"
