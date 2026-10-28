# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/bluez_runtime.py build/host-dev
from __future__ import annotations
import socket
from typing import Final
from gi.repository import Gio, GLib

AA: Final = "4de17a00-52cb-11e6-bdf4-0800200c9a66"
HFP: Final = "0000111f-0000-1000-8000-00805f9b34fb"
ROOT: Final = "/org/custom/AndroidAutoSystemHelper"
ADAPTER: Final = "/org/bluez/hci0"
DEVICE: Final = ADAPTER + "/dev_02_00_00_00_00_01"
OTHER: Final = ADAPTER + "/dev_02_00_00_00_00_02"
FLAGS: Final = Gio.DBusCallFlags.NO_AUTO_START


class ProfileSockets:
    helper: str
    peer_socket: socket.socket | None = None
    hfp_socket: socket.socket | None = None

    def deliver(self, bus: Gio.DBusConnection, path: str, words: list[str]) -> None:
        local, candidate = socket.socketpair()
        candidate.settimeout(0.2)
        is_hfp = "hfp" in words
        previous = self.hfp_socket if is_hfp else self.peer_socket
        with local:
            fds = Gio.UnixFDList.new()
            with open("/dev/null", "rb") as regular:
                index = fds.append(regular.fileno() if "regular" in words else local.fileno())
            if "bad-index" in words:
                index += 1
            try:
                bus.call_with_unix_fd_list_sync(self.helper, path, "org.bluez.Profile1", "NewConnection",
                    GLib.Variant("(oha{sv})", (OTHER if "wrong" in words else DEVICE, index, {})),
                    None, FLAGS, 2000, None if "missing-fd" in words else fds, None)
            except GLib.Error:
                candidate.close()
                raise
        if previous is not None:
            previous.close()
        if is_hfp:
            self.hfp_socket = candidate
        else:
            self.peer_socket = candidate

    def hfp_wire(self, text: str) -> str:
        assert self.hfp_socket is not None
        if text.startswith("hfp-wire-send "):
            self.hfp_socket.sendall(bytes.fromhex(text.removeprefix("hfp-wire-send ")))
            return "ok"
        try:
            payload = self.hfp_socket.recv(8192)
            return payload.hex() if payload else "closed"
        except TimeoutError:
            return "open"
