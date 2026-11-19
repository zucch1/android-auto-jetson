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
HFP: Final = "0000111e-0000-1000-8000-00805f9b34fb"
ROOT: Final = "/org/custom/AndroidAutoSystemHelper"
ADAPTER: Final = "/org/bluez/hci0"
DEVICE: Final = ADAPTER + "/dev_02_00_00_00_00_01"
OTHER: Final = ADAPTER + "/dev_02_00_00_00_00_02"
FLAGS: Final = Gio.DBusCallFlags.NO_AUTO_START


class ProfileSockets:
    """Profile1 NewConnection sockets, modelled on bluetoothd (BlueZ 5.72 src/profile.c).

    `local` is the daemon's side. bluetoothd keeps its own reference to the accepted RFCOMM
    socket (create_conn: conn->io) after handing the helper a dup, and drops it only when the
    socket hangs up, NewConnection fails, or the profile goes away. So the mock keeps `local`
    open too: the phone side (`candidate`) reads EOF only when the helper shuts the socket
    down, never because the helper merely closed its own fd (#161).

    AF_UNIX vs RFCOMM: shutdown(SHUT_RDWR) on any fd of one socketpair end marks that socket
    shut down and gives the peer EOF while other fds of that end stay open, as RFCOMM's
    shutdown sends DISC on the DLC while bluetoothd's fd stays open. close() of one of several
    fds does neither, in both families. Differences that the mock does not model: RFCOMM DISC
    also makes bluetoothd's own watch fire (HUP) and drop its reference, and the socketpair
    peer can still read data queued before the shutdown.
    """

    helper: str
    peer_socket: socket.socket | None = None
    hfp_socket: socket.socket | None = None
    peer_daemon: socket.socket | None = None
    hfp_daemon: socket.socket | None = None

    def deliver(self, bus: Gio.DBusConnection, path: str, words: list[str]) -> None:
        local, candidate = socket.socketpair()
        candidate.settimeout(0.2)
        is_hfp = "hfp" in words
        previous = self.hfp_socket if is_hfp else self.peer_socket
        previous_daemon = self.hfp_daemon if is_hfp else self.peer_daemon
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
            # An error reply makes bluetoothd drop its reference (new_conn_reply).
            local.close()
            candidate.close()
            raise
        for stale in (previous, previous_daemon):
            if stale is not None:
                stale.close()
        if is_hfp:
            self.hfp_socket, self.hfp_daemon = candidate, local
        else:
            self.peer_socket, self.peer_daemon = candidate, local

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
