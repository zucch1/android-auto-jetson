# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/helper/bluez_runtime.py build/host-dev
from __future__ import annotations

import json
import socket
import sys
import threading
from typing import Final

from gi.repository import Gio, GLib

AA: Final = "4de17a00-52cb-11e6-bdf4-0800200c9a66"
ADAPTER: Final = "/org/bluez/hci0"
DEVICE: Final = ADAPTER + "/dev_02_00_00_00_00_01"
OTHER: Final = ADAPTER + "/dev_02_00_00_00_00_02"
ROOT: Final = "/org/custom/AndroidAutoSystemHelper"
FLAGS: Final = Gio.DBusCallFlags.NO_AUTO_START


def connect(address: str) -> Gio.DBusConnection:
    return Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None, None,
    )


class MockBluez:
    def __init__(self, address: str) -> None:
        self.bus = connect(address)
        self.rogue = connect(address)
        self.helper = ""
        self.paired = False
        self.bonded = False
        self.multiple = False
        self.present = True
        self.observed_device = DEVICE
        self.fail_remove = False
        self.fail_set = False
        self.slow_inventory = False
        self.removed: list[str] = []
        self.properties = {"Pairable": True, "Discoverable": True}
        self.profile: list[str] = []
        self.default_agent = False
        self.peer_socket: socket.socket | None = None
        xml = """<node>
<interface name="org.freedesktop.DBus.ObjectManager"><method name="GetManagedObjects"><arg type="a{oa{sa{sv}}}" direction="out"/></method></interface>
<interface name="org.bluez.ProfileManager1"><method name="RegisterProfile"><arg type="o" direction="in"/><arg type="s" direction="in"/><arg type="a{sv}" direction="in"/></method><method name="UnregisterProfile"><arg type="o" direction="in"/></method></interface>
<interface name="org.bluez.AgentManager1"><method name="RegisterAgent"><arg type="o" direction="in"/><arg type="s" direction="in"/></method><method name="RequestDefaultAgent"><arg type="o" direction="in"/></method><method name="UnregisterAgent"><arg type="o" direction="in"/></method></interface>
<interface name="org.freedesktop.DBus.Properties"><method name="Set"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="in"/></method></interface>
<interface name="org.bluez.Adapter1"><method name="RemoveDevice"><arg type="o" direction="in"/></method></interface>
<interface name="org.bluez.Device1"><method name="CancelPairing"/></interface>
</node>"""
        info = Gio.DBusNodeInfo.new_for_xml(xml)
        for index, path in enumerate(["/", "/org/bluez", "/org/bluez", ADAPTER, ADAPTER, DEVICE]):
            self.bus.register_object(path, info.interfaces[index], self.method, None, None)
        self.name("RequestName", GLib.Variant("(su)", ("org.bluez", 0)))

    def name(self, method: str, args: GLib.Variant) -> None:
        self.bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           method, args, None, FLAGS, 1000, None)

    def inventory(self) -> GLib.Variant:
        objects = {ADAPTER: {"org.bluez.Adapter1": {"Address": GLib.Variant("s", "02:00:00:00:00:AA")}}}
        paths = ([self.observed_device] if self.present else []) + ([OTHER] if self.multiple else [])
        for path in paths:
            objects[path] = {"org.bluez.Device1": {
                "Address": GLib.Variant("s", "02:00:00:00:00:01" if path == DEVICE else "02:00:00:00:00:02"),
                "AddressType": GLib.Variant("s", "public"), "Adapter": GLib.Variant("o", ADAPTER),
                "Paired": GLib.Variant("b", self.paired),
                "Bonded": GLib.Variant("b", self.bonded),
            }}
        return GLib.Variant("(a{oa{sa{sv}}})", (objects,))

    def method(self, _bus: Gio.DBusConnection, sender: str, _path: str, _interface: str,
               method: str, args: GLib.Variant, invocation: Gio.DBusMethodInvocation) -> None:
        match method:
            case "GetManagedObjects":
                if self.slow_inventory:
                    def delayed() -> bool:
                        invocation.return_value(self.inventory())
                        return False
                    GLib.timeout_add(500, delayed)
                    return
                invocation.return_value(self.inventory())
                return
            case "Set":
                if self.fail_set:
                    invocation.return_dbus_error("org.bluez.Error.Failed", "fixture Set failure")
                    return
                _, prop, value = args.unpack()
                self.properties[prop] = value
            case "RegisterProfile":
                path, uuid, options = args.unpack()
                assert uuid == AA and path == ROOT + "/aa_profile"
                assert options["Role"] == "server" and options["Channel"] == 8
                assert options["RequireAuthentication"] and options["RequireAuthorization"]
                assert not options["AutoConnect"]
                self.helper = sender
                self.profile = [uuid]
            case "RequestDefaultAgent":
                self.default_agent = True
            case "RegisterAgent":
                assert args.unpack() == (ROOT + "/aa_agent", "NoInputNoOutput")
            case "RemoveDevice":
                if self.fail_remove:
                    invocation.return_dbus_error("org.bluez.Error.Failed", "fixture RemoveDevice failure")
                    return
                self.removed.append(args.unpack()[0])
                self.present = False
                self.paired = False
            case "UnregisterProfile":
                self.profile = []
            case "UnregisterAgent" | "CancelPairing":
                pass
            case _:
                invocation.return_dbus_error("org.bluez.Error.NotSupported", "fixture method")
                return
        invocation.return_value(None)

    def callback(self, command: str) -> str:
        words = command.split()
        member = words[0]
        bus = self.rogue if "rogue" in words else self.bus
        device = OTHER if "wrong" in words else DEVICE
        interface = "org.bluez.Agent1"
        path = ROOT + "/aa_agent"
        params = GLib.Variant("(o)", (device,))
        match member:
            case "AuthorizeService":
                params = GLib.Variant("(os)", (device, "0000110b-0000-1000-8000-00805f9b34fb" if "audio" in words else AA))
            case "RequestDisconnection" | "NewConnection":
                interface, path = "org.bluez.Profile1", ROOT + "/aa_profile"
            case "Cancel" | "Release":
                params = None
            case "RequestAuthorization":
                pass
            case "RequestConfirmation":
                params = GLib.Variant("(ou)", (device, 123456))
            case _:
                raise AssertionError(command)
        try:
            if member == "NewConnection":
                if self.peer_socket is not None:
                    self.peer_socket.close()
                local, self.peer_socket = socket.socketpair()
                self.peer_socket.settimeout(0.2)
                with local:
                    fds = Gio.UnixFDList.new()
                    index = fds.append(local.fileno())
                    bus.call_with_unix_fd_list_sync(self.helper, path, interface, member,
                        GLib.Variant("(oha{sv})", (device, index, {})), None, FLAGS, 2000, fds, None)
            else:
                bus.call_sync(self.helper, path, interface, member, params, None, FLAGS, 2000, None)
            return "allowed"
        except GLib.Error as error:
            assert "org.bluez.Error.Rejected" in str(error), str(error)
            return "rejected"

    def command(self, text: str) -> str:
        match text:
            case "state":
                return json.dumps({"properties": self.properties, "profile": self.profile,
                    "default_agent": self.default_agent, "removed": self.removed, "helper": self.helper})
            case "multiple": self.multiple = True
            case "single": self.multiple = False
            case "observe-other": self.observed_device = OTHER
            case "observe-first": self.observed_device = DEVICE
            case "paired": self.paired = True; self.bonded = True
            case "temporary-pair": self.paired = True; self.bonded = False
            case "unpaired": self.paired = False; self.bonded = False
            case "restore": self.present = True
            case "fail-remove": self.fail_remove = True
            case "allow-remove": self.fail_remove = False
            case "fail-set": self.fail_set = True
            case "allow-set": self.fail_set = False
            case "slow-inventory": self.slow_inventory = True
            case "fast-inventory": self.slow_inventory = False
            case "release-name": self.name("ReleaseName", GLib.Variant("(s)", ("org.bluez",)))
            case "acquire-name": self.name("RequestName", GLib.Variant("(su)", ("org.bluez", 0)))
            case "fd-eof":
                assert self.peer_socket is not None
                try:
                    return "closed" if self.peer_socket.recv(1) == b"" else "data"
                except TimeoutError:
                    return "open"
            case _:
                return self.callback(text)
        return "ok"


def main() -> None:
    mock = MockBluez(sys.argv[1])
    loop = GLib.MainLoop()
    worker = threading.Thread(target=loop.run, daemon=True)
    worker.start()
    print("ready", flush=True)
    for command in sys.stdin:
        print(mock.command(command.strip()), flush=True)
    loop.quit()
    worker.join(timeout=2)


if __name__ == "__main__":
    main()
