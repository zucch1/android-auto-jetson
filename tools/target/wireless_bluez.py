# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
# Run: loaded on target by wireless_target.py; uses existing system bindings only.
"""Temporary custom profile and rejecting agent; no pairing or trust actions."""
from contextlib import contextmanager
from collections.abc import Iterator
import importlib
from typing import TYPE_CHECKING, Protocol

if TYPE_CHECKING:
    from wireless_target import Report


class EventContext(Protocol):
    def iteration(self, may_block: bool) -> bool: ...


@contextmanager
def registration(report: 'Report') -> Iterator[EventContext]:
    dbus = importlib.import_module('dbus')
    service = importlib.import_module('dbus.service')
    mainloop = importlib.import_module('dbus.mainloop.glib')
    glib = importlib.import_module('gi.repository.GLib')
    mainloop.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    class Rejected(dbus.DBusException):
        _dbus_error_name = 'org.bluez.Error.Rejected'

    class Profile(service.Object):
        @service.method('org.bluez.Profile1', in_signature='', out_signature='')
        def Release(self):
            report.agent_callbacks.append('profile-release')

        @service.method('org.bluez.Profile1', in_signature='oha{sv}', out_signature='')
        def NewConnection(self, device, fd, properties):
            raise Rejected('Capability probe does not accept Bluetooth connections')

        @service.method('org.bluez.Profile1', in_signature='o', out_signature='')
        def RequestDisconnection(self, device):
            report.agent_callbacks.append('profile-disconnection')

    class Agent(service.Object):
        @service.method('org.bluez.Agent1', in_signature='', out_signature='')
        def Release(self):
            report.agent_callbacks.append('release')

        @service.method('org.bluez.Agent1', in_signature='', out_signature='')
        def Cancel(self):
            report.agent_callbacks.append('cancel')

        @service.method('org.bluez.Agent1', in_signature='o', out_signature='s')
        def RequestPinCode(self, device):
            report.agent_callbacks.append('request-pin-code-rejected')
            raise Rejected('Pairing deferred to task 36')

        @service.method('org.bluez.Agent1', in_signature='o', out_signature='u')
        def RequestPasskey(self, device):
            report.agent_callbacks.append('request-passkey-rejected')
            raise Rejected('Pairing deferred to task 36')

        @service.method('org.bluez.Agent1', in_signature='ou', out_signature='')
        def RequestConfirmation(self, device, passkey):
            report.agent_callbacks.append('request-confirmation-rejected')
            raise Rejected('Pairing deferred to task 36')

        @service.method('org.bluez.Agent1', in_signature='o', out_signature='')
        def RequestAuthorization(self, device):
            report.agent_callbacks.append('request-authorization-rejected')
            raise Rejected('Pairing deferred to task 36')

        @service.method('org.bluez.Agent1', in_signature='os', out_signature='')
        def AuthorizeService(self, device, service_uuid):
            report.agent_callbacks.append('authorize-service-rejected')
            raise Rejected('Trust deferred to task 36')

        @service.method('org.bluez.Agent1', in_signature='os', out_signature='')
        def DisplayPinCode(self, device, pin):
            report.agent_callbacks.append('display-pin-code')

        @service.method('org.bluez.Agent1', in_signature='ouq', out_signature='')
        def DisplayPasskey(self, device, passkey, entered):
            report.agent_callbacks.append('display-passkey')

    profile_path, agent_path = '/aa/task12/profile', '/aa/task12/agent'
    profile, agent = Profile(bus, profile_path), Agent(bus, agent_path)
    manager = bus.get_object('org.bluez', '/org/bluez')
    profiles = dbus.Interface(manager, 'org.bluez.ProfileManager1')
    agents = dbus.Interface(manager, 'org.bluez.AgentManager1')
    try:
        profiles.RegisterProfile(profile_path, 'b4c3f081-7811-4a2b-b2c0-28ef47a7e812',
                                 {'Name': 'aa-task12-capability', 'Role': 'server',
                                  'RequireAuthentication': True, 'RequireAuthorization': True}, timeout=10)
        report.bluez_profile_registration = True
        agents.RegisterAgent(agent_path, 'KeyboardDisplay', timeout=10)
        report.bluez_agent_registration = True
        yield glib.MainContext.default()
    except dbus.DBusException as error:
        report.blockers.append('bluez-registration-failed')
        report.raw['bluez_error'] = error.get_dbus_name()
        raise OSError(error.get_dbus_name()) from error
    finally:
        try:
            if report.bluez_agent_registration:
                agents.UnregisterAgent(agent_path, timeout=10)
            if report.bluez_profile_registration:
                profiles.UnregisterProfile(profile_path, timeout=10)
        finally:
            profile.remove_from_connection()
            agent.remove_from_connection()
            bus.close()
