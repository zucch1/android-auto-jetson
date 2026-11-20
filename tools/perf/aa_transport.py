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
runs ``adb tcpip 5555`` once beforehand while the phone is cabled (runbook).
The phone adb drives must be the phone the receiver admitted under
``--phone-id``, so the harness does not pick "some station on the AP". It
takes the peer address of the receiver's own ESTABLISHED TCP session
(``receiver_session_peers``: the receiver process's socket inodes matched in
its ``/proc/PID/net/tcp``), requires exactly one such peer and that it is a
station of the AP (DHCP lease or neighbour on the AP interface; IPv4 only,
MACs, the PSK and the SSID are never read into the record or printed). The AP
interface is given by the operator (``--ap-interface``): the helper's
``ap.conf`` is root-only (0600) and sits beside the AP credentials, so the
harness never reads it. The harness then runs
``adb connect <peer>:5555`` and binds only when the device itself answers
``getprop ro.serialno`` with the expected phone serial (``bind_adb``).
``adb get-serialno`` is not used: it is a host query that, for a TCP
transport, returns the endpoint ``<ip>:5555``. The chain is: the receiver
admits only ``--phone-id``'s stored wireless identity, the session socket's
peer is the phone holding that admitted session, and adb reaches that same
address and reports the expected hardware serial. Anything else BLOCKS the
run (``adb_session_*``, ``adb_unreachable``, ``adb_phone_mismatch``). The
phone's own link state (USB functions, Wi-Fi frequency/channel/RSSI,
Bluetooth) is then read over that adb into the record.

A result records ``transport``. A gate asked for one transport refuses a record
of the other, and a record without the field is wired (every record before this
field existed came from the wired receiver).
"""
from __future__ import annotations

import ipaddress
import os
import re
import socket
import struct
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

NM_LEASES = "/var/lib/NetworkManager/dnsmasq-{interface}.leases"
ADB_TCP_PORT = 5555
_INTERFACE = re.compile(r"[A-Za-z0-9_.-]{1,15}")
# Non-DFS UNII-1 channels aa-wireless-receiver and the helper accept (--ap-channel).
AP_CHANNELS = (36, 40, 44, 48)


def valid_interface(name: object) -> bool:
    """A plausible Linux interface name (``--ap-interface``), never a path or option."""
    return isinstance(name, str) and not name.startswith(("-", ".")) and bool(_INTERFACE.fullmatch(name))


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


PROC = Path("/proc")
_SOCKET_LINK = re.compile(r"socket:\[(\d+)\]")
_TCP_ESTABLISHED = "01"


def _proc_address(hex_address: str) -> str | None:
    """The IPv4 address of a /proc/net/tcp{,6} ``ADDR:PORT`` field (host byte order words)."""
    address = hex_address.split(":")[0]
    if len(address) not in (8, 32):
        return None
    try:
        raw = b"".join(struct.pack("=I", int(address[i:i + 8], 16)) for i in range(0, len(address), 8))
    except ValueError:
        return None
    if len(raw) == 4:
        return socket.inet_ntop(socket.AF_INET, raw)
    if len(raw) == 16:
        address6 = ipaddress.IPv6Address(raw)
        # A native IPv6 peer is returned as such, never dropped: the caller
        # must not mistake "one IPv4 peer and one IPv6 peer" for one peer.
        return str(address6.ipv4_mapped or address6)
    return None


def receiver_session_peers(pid: int, proc: Path | None = None) -> list[str] | None:
    """IPv4 peers of the ESTABLISHED TCP sockets process ``pid`` owns; None if unreadable.

    Read-only procfs: the socket inodes of ``/proc/PID/fd`` matched against
    ``/proc/PID/net/tcp`` and ``tcp6`` (the process's own network namespace).
    The receiver's only TCP sockets are its Android Auto session (its IPC and
    the helper are Unix sockets), so its peer is the admitted phone.
    """
    proc = PROC if proc is None else proc  # module attribute: test seam
    base = proc / str(pid)
    try:
        inodes = set()
        for entry in (base / "fd").iterdir():
            try:
                match = _SOCKET_LINK.fullmatch(os.readlink(entry))
            except OSError:
                continue
            if match:
                inodes.add(match.group(1))
    except OSError:
        return None
    peers: list[str] = []
    for table in ("tcp", "tcp6"):
        try:
            lines = (base / "net" / table).read_text().splitlines()[1:]
        except FileNotFoundError:
            if table == "tcp6":
                continue  # a kernel without IPv6 has no tcp6 table
            return None
        except OSError:
            return None  # an unreadable table is not an empty one
        for line in lines:
            fields = line.split()
            if len(fields) < 10 or fields[3] != _TCP_ESTABLISHED or fields[9] not in inodes:
                continue
            peer = _proc_address(fields[2])
            if peer is None:
                return None  # a session socket we cannot decode: unknown, not absent
            if peer not in peers:
                peers.append(peer)
    return peers


def session_candidate(peers: list[str] | None, stations: list[str]) -> tuple[str | None, str | None]:
    """The one address adb may bind: the receiver session's peer, if it is an AP station.

    (address, None) or (None, reason): ``adb_session_unreadable`` (procfs could
    not be read), ``adb_session_peer_unknown`` (no established session socket),
    ``adb_session_ambiguous`` (more than one peer, or any native IPv6 peer: the
    AP hands out IPv4 only, so an IPv6 session cannot be tied to a station),
    ``adb_session_not_on_ap`` (the peer is no station of the helper's AP).
    """
    if peers is None:
        return None, "adb_session_unreadable"
    if not peers:
        return None, "adb_session_peer_unknown"
    if len(peers) > 1 or any(":" in peer for peer in peers):
        return None, "adb_session_ambiguous"
    if peers[0] not in stations:
        return None, "adb_session_not_on_ap"
    return peers[0], None


def link_gap(link: object) -> str | None:
    """Why a wireless link record does not prove a bound, cable-out session; None if it does.

    adb must have been bound (``adb_bound`` true) and this host's USB bus must
    have been readable and clean (no phone, no AOA device) before, at every
    sample during (at least one), and after the run. Shared by the drive
    smoke's soak gate and the perf reporter.
    """
    if not isinstance(link, dict):
        return "wireless_link_unrecorded"
    if link.get("adb_bound") is not True:
        return "wireless_adb_unbound"
    host = link.get("host_usb")
    if not isinstance(host, dict) or not isinstance(host.get("during"), list) or not host["during"]:
        return "wireless_host_usb_unrecorded"
    for moment, evidence in ([("before", host.get("before"))] + [("during", e) for e in host["during"]]
                             + [("after", host.get("after"))]):
        refusal = host_usb_refusal(evidence)
        if refusal:
            return f"wireless_{refusal.split(':')[-1]}_{moment}"
    return None


# `adb connect` success: "connected to T" or "already connected to T", nothing else
# ("disconnected", "failed to connect", "failed to authenticate", "cannot ...").
_CONNECTED = re.compile(r"(?:already )?connected to \S+$")


def bind_adb(expected_serial: str, candidates: list[str],
             adb: Callable[..., object]) -> tuple[str | None, str | None]:
    """Connect adb to the candidate that is the expected phone: (adb target, None) or (None, reason).

    ``adb(*args)`` runs one bounded host adb command. Each candidate is tried as
    ``<ip>:5555``; it is bound only when the device answers ``shell getprop
    ro.serialno`` with ``expected_serial``. (``adb -s <ip>:5555 get-serialno``
    asks the host server and returns the endpoint itself for a TCP transport,
    so it proves nothing.) A candidate that answers with another serial is
    disconnected again. Reasons: ``adb_unreachable`` (no candidate, or none
    answered), ``adb_phone_mismatch`` (only other phones answered).
    """
    other = False
    for address in candidates:
        target = f"{address}:{ADB_TCP_PORT}"
        connected = adb("connect", target)
        text = getattr(connected, "stdout", "") or ""
        if getattr(connected, "returncode", 1) != 0 or not _CONNECTED.match(text.strip()):
            # "failed to connect", "failed to authenticate" (adb exits 0 for
            # some of these) and the like: drop whatever transport it left.
            adb("disconnect", target)
            continue
        answer = adb("-s", target, "shell", "getprop", "ro.serialno")
        serial = (getattr(answer, "stdout", "") or "").strip() if getattr(answer, "returncode", 1) == 0 else ""
        if serial and serial == expected_serial:
            return target, None
        if serial:
            other = True
        adb("disconnect", target)
    return None, "adb_phone_mismatch" if other else "adb_unreachable"
