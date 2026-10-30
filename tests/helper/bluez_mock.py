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
from gi.repository import Gio, GLib
from bluez_profile_mock import AA, HFP, ADAPTER, DEVICE, OTHER, ROOT, FLAGS, ProfileSockets

def connect(address: str) -> Gio.DBusConnection:
    return Gio.DBusConnection.new_for_address_sync(
        address, Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION,
        None, None,
    )


class MockBluez(ProfileSockets):
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
        self.owner_during_inventory = False
        self.removed: list[str] = []
        self.cancel_unpaired = 0
        self.properties = {"Pairable": True, "Discoverable": True}
        self.profile: list[str] = []
        self.profile_senders: dict[str, str] = {}
        self.fail_register: set[str] = set()
        self.late_register: set[str] = set()
        self.register_attempts = {AA: 0, HFP: 0}
        self.default_agent = False
        self.comparison_result = "pending"
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
                if self.owner_during_inventory:
                    self.owner_during_inventory = False
                    self.name("ReleaseName", GLib.Variant("(s)", ("org.bluez",)))
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
                assert (uuid, path, options["Channel"]) == (AA, ROOT + "/aa_profile", 8)
                assert options["Role"] == "server"
                assert options["RequireAuthentication"] and options["RequireAuthorization"]
                assert not options["AutoConnect"]
                self.helper = sender
                self.register_attempts[uuid] += 1
                if uuid in self.profile:
                    previous = self.profile_senders[uuid]
                    alive = self.bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                        "org.freedesktop.DBus", "NameHasOwner", GLib.Variant("(s)", (previous,)),
                        None, FLAGS, 1000, None).unpack()[0]
                    if alive:
                        invocation.return_dbus_error("org.bluez.Error.AlreadyExists", "profile already registered")
                        return
                    self.profile.remove(uuid)
                if uuid in self.fail_register:
                    invocation.return_dbus_error("org.bluez.Error.Failed", "fixture RegisterProfile failure")
                    return
                self.profile.append(uuid)
                self.profile_senders[uuid] = sender
                if uuid in self.late_register:
                    self.late_register.remove(uuid)
                    invocation.return_dbus_error("org.bluez.Error.Timeout", "fixture late successful registration")
                    return
            case "RequestDefaultAgent":
                self.default_agent = True
            case "RegisterAgent":
                assert args.unpack() == (ROOT + "/aa_agent", "DisplayYesNo")
            case "RemoveDevice":
                if self.fail_remove:
                    invocation.return_dbus_error("org.bluez.Error.Failed", "fixture RemoveDevice failure")
                    return
                self.removed.append(args.unpack()[0])
                self.present = False
                self.paired = False
            case "UnregisterProfile":
                uuid = HFP if args.unpack()[0] == ROOT + "/hfp_ag_profile" else AA
                if uuid in self.profile:
                    self.profile.remove(uuid)
                self.profile_senders.pop(uuid, None)
            case "CancelPairing":
                # BlueZ answers CancelPairing without an in-flight Pair() with MGMT Unpair Device.
                if self.paired:
                    self.cancel_unpaired += 1
                self.paired = False
                self.bonded = False
            case "UnregisterAgent":
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
        if "hfp" in words or "aa-profile" in words:
            interface = "org.bluez.Profile1"
            path = ROOT + ("/hfp_ag_profile" if "hfp" in words else "/aa_profile")
        match member:
            case "AuthorizeService":
                interface, path = "org.bluez.Agent1", ROOT + "/aa_agent"
                uuid = HFP if "hfp" in words else AA
                if "audio" in words: uuid = "0000110b-0000-1000-8000-00805f9b34fb"
                if "ag" in words: uuid = "0000111f-0000-1000-8000-00805f9b34fb"
                params = GLib.Variant("(os)", (device, uuid))
            case "RequestDisconnection" | "NewConnection":
                interface = "org.bluez.Profile1"
                path = ROOT + ("/hfp_ag_profile" if "hfp" in words else "/aa_profile")
            case "Cancel" | "Release":
                params = None
            case "RequestAuthorization" | "RequestPinCode" | "RequestPasskey":
                pass
            case "RequestConfirmation":
                params = GLib.Variant("(ou)", (device, 1000000 if "overflow" in words else 42))
            case _:
                raise AssertionError(command)
        try:
            if member == "NewConnection":
                self.deliver(bus, path, words)
            else:
                if member == "Release" and interface == "org.bluez.Profile1" and bus is self.bus:
                    self.profile.remove(HFP if "hfp" in words else AA)
                bus.call_sync(self.helper, path, interface, member, params, None, FLAGS,
                              70000 if member == "RequestConfirmation" else 2000, None)
            return "allowed"
        except GLib.Error as error:
            assert "org.bluez.Error.Rejected" in str(error), str(error)
            return "rejected"

    def command(self, text: str) -> str:
        if text.startswith("hfp-wire-"):
            return self.hfp_wire(text)
        if text.startswith("comparison-start"):
            self.comparison_result = "pending"
            callback = text.replace("comparison-start", "RequestConfirmation", 1)
            def compare() -> None:
                self.comparison_result = self.callback(callback)
            threading.Thread(target=compare, daemon=True).start()
            return "ok"
        if text == "comparison-result":
            return self.comparison_result
        if text.startswith("wire-send "):
            assert self.peer_socket is not None
            self.peer_socket.sendall(bytes.fromhex(text.removeprefix("wire-send ")))
            return "ok"
        match text:
            case "wire-read":
                assert self.peer_socket is not None
                try:
                    payload = self.peer_socket.recv(8192)
                    return payload.hex() if payload else "closed"
                except TimeoutError:
                    return "open"
            case "state":
                return json.dumps({"properties": self.properties, "profile": self.profile,
                    "default_agent": self.default_agent, "removed": self.removed, "helper": self.helper,
                    "cancel_unpaired": self.cancel_unpaired,
                    "register_attempts": self.register_attempts})
            case "multiple": self.multiple = True
            case "single": self.multiple = False
            case "observe-other": self.observed_device = OTHER
            case "observe-first": self.observed_device = DEVICE
            case "paired": self.paired = True; self.bonded = True
            case "temporary-pair": self.paired = True; self.bonded = False
            case "unpaired": self.paired = False; self.bonded = False
            case "restore": self.present = True
            case "remove": self.present = False
            case "owner-during-inventory": self.owner_during_inventory = True
            case "recreate":
                self.bus.emit_signal(None, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                                     GLib.Variant("(oas)", (DEVICE, ["org.bluez.Device1"])))
            case "adapter-recreate":
                self.bus.emit_signal(None, "/", "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                                     GLib.Variant("(oas)", (ADAPTER, ["org.bluez.Adapter1"])))
            case "bond-cycle":
                self.bus.emit_signal(None, DEVICE, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                     GLib.Variant("(sa{sv}as)", ("org.bluez.Device1", {"Bonded": GLib.Variant("b", False)}, [])))
            case "connected-false" | "connected-true" | "connected-invalidated":
                self.bus.emit_signal(None, DEVICE, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                    GLib.Variant("(sa{sv}as)", ("org.bluez.Device1",
                        {} if text == "connected-invalidated" else {"Connected": GLib.Variant("b", text == "connected-true")},
                        ["Connected"] if text == "connected-invalidated" else [])))
            case "wire-eof":
                assert self.peer_socket is not None
                self.peer_socket.shutdown(socket.SHUT_WR)
            case "wire-shutdown-read":
                assert self.peer_socket is not None
                self.peer_socket.shutdown(socket.SHUT_RD)
            case "fail-remove": self.fail_remove = True
            case "allow-remove": self.fail_remove = False
            case "fail-set": self.fail_set = True
            case "allow-set": self.fail_set = False
            case "fail-register-hfp": self.fail_register.add(HFP)
            case "allow-register-hfp": self.fail_register.discard(HFP)
            case "late-register-hfp": self.late_register.add(HFP)
            case "slow-inventory": self.slow_inventory = True
            case "fast-inventory": self.slow_inventory = False
            case "release-name":
                self.name("ReleaseName", GLib.Variant("(s)", ("org.bluez",)))
                self.profile.clear()
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
