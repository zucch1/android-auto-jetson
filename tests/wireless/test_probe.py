# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wireless/test_probe.py [policy|discovery|active|cli|window|bluez]
"""Hardware boundaries are mocked: this suite cannot contact a target or phone."""
import argparse
from contextlib import contextmanager, redirect_stdout
from dataclasses import asdict
import io
import json
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/target'))
import wireless_bluez as bluez
import wireless_probe as host
import wireless_target as target

INFO = 'Supported interface modes:\n\t * managed\n\t * AP\n\t * 5180 MHz [36] (23.0 dBm)\n'
REG = 'global\ncountry US: DFS-FCC\n\t(5170 - 5250 @ 80), (N/A, 23), (N/A), AUTO-BW\n'
BUS = 'org.bluez.ProfileManager1\norg.bluez.AgentManager1\n'


class Policy(unittest.TestCase):
    def test_confirmed_us(self):
        self.assertTrue(target.eligible(INFO, target.regulatory_section(REG, 'phy0'), 'US'))

    def test_unconfirmed(self):
        self.assertFalse(target.eligible(INFO, target.regulatory_section(REG, 'phy0'), ''))

    def test_effective_mismatch(self):
        self.assertFalse(target.eligible(INFO, target.regulatory_section(REG, 'phy0'), 'CA'))

    def test_channel_restrictions(self):
        for flag in ('disabled', 'no IR', 'radar detection', 'DFS', 'passive scanning', 'no 20MHz', 'no OFDM'):
            with self.subTest(flag=flag):
                self.assertFalse(target.eligible(INFO.replace('(23.0 dBm)', flag), REG[7:], 'US'))

    def test_regulatory_restrictions(self):
        for flag in ('NO-IR', 'DFS', 'NO-OUTDOOR', 'NO-20MHZ', 'NO-OFDM'):
            with self.subTest(flag=flag):
                self.assertFalse(target.eligible(INFO, REG[7:].rstrip() + ', ' + flag, 'US'))

    def test_missing_or_duplicate_channel(self):
        for info in ('', INFO + INFO):
            self.assertFalse(target.eligible(info, REG[7:], 'US'))

    def test_bandwidth_and_range(self):
        for reg in (REG.replace('5170', '5180'), REG.replace('5250', '5185'), REG.replace('@ 80', '@ 10')):
            self.assertFalse(target.eligible(INFO, reg[7:], 'US'))

    def test_self_managed_phy_is_authoritative(self):
        reg = REG + 'phy#0 (self-managed)\ncountry 00: DFS-UNSET\n (5170 - 5250 @ 80), NO-IR\n'
        self.assertFalse(target.eligible(INFO, target.regulatory_section(reg, 'phy0'), 'US'))

    def test_other_phy_does_not_override(self):
        self.assertIn('COUNTRY US:', target.regulatory_section(REG + REG.replace('global', 'phy#1').replace('US', 'CA'), 'phy0'))


class Discovery(unittest.TestCase):
    def check_discovery(self, info=INFO, reg=REG, bus=BUS, jurisdiction='US'):
        report = target.Report(confirmed_jurisdiction=jurisdiction)
        paths = [Path('/sys/phy0'), Path('/drivers/rtw_8822ce')]
        with patch.object(Path, 'resolve', side_effect=paths), patch.object(target, 'command',
                side_effect=[info, reg, 'GENERAL.STATE:30 (disconnected)', bus]):
            target.discover(report, 'wlan0')
        return report

    def test_happy_discovery_not_registration(self):
        report = self.check_discovery()
        self.assertEqual(report.blockers, [])
        self.assertFalse(report.bluez_profile_registration)

    def test_missing_ap(self):
        self.assertIn('missing-ap-capability', self.check_discovery(info=INFO.replace('* AP', '* managed')).blockers)

    def test_ineligible_channel(self):
        self.assertIn('channel-36-ineligible', self.check_discovery(info=INFO.replace('23.0 dBm', 'no IR')).blockers)

    def test_missing_profile(self):
        self.assertIn('missing-bluez-profile-capability', self.check_discovery(bus='org.bluez.AgentManager1').blockers)

    def test_unconfirmed_inventory(self):
        report = self.check_discovery(jurisdiction='')
        self.assertEqual(report.effective_regulatory_domain, 'US')
        self.assertIn('jurisdiction-unconfirmed', report.blockers)


@contextmanager
def fake_registration(report):
    report.bluez_profile_registration = report.bluez_agent_registration = True
    yield SimpleNamespace(iteration=lambda block: None)


class Active(unittest.TestCase):
    def test_blockers_prevent_mutation(self):
        for blocker in ('jurisdiction-unconfirmed', 'missing-ap-capability', 'channel-36-ineligible',
                        'missing-bluez-profile-capability'):
            report = target.Report(blockers=[blocker])
            with patch.object(target, 'command') as command:
                target.active(report, argparse.Namespace())
            command.assert_not_called()

    def test_isolation_gate(self):
        report = target.Report(raw={'nm': 'GENERAL.STATE:100 (connected)'})
        with patch.object(target, 'command') as command:
            target.active(report, argparse.Namespace())
        command.assert_not_called()
        self.assertIn('interface-not-isolated-disconnected', report.blockers)

    def exercise(self, failure=False, authorized=True):
        report = target.Report(raw={'nm': 'GENERAL.STATE:30 (disconnected)'})
        calls = []
        def run(argv, payload=None):
            calls.append(argv)
            if 'up' in argv and failure:
                raise subprocess.CalledProcessError(1, argv)
            if argv[:2] == ['iw', 'dev']:
                return 'type AP\nchannel 36 (5180 MHz)' if argv[-1] == 'info' else ('Station aa:bb:cc:dd:ee:ff (on wlan0)\n\tauthorized: ' + ('yes' if authorized else 'no') + '\n')
            return ''
        args = argparse.Namespace(interface='wlan0', duration=1, ap_psk='fixture-only-password')
        with patch.object(target, 'command', side_effect=run), patch.object(bluez, 'registration', fake_registration), \
                patch.object(target.time, 'monotonic', side_effect=[0, 0, 2]), patch.object(target.time, 'sleep'):
            if failure:
                with self.assertRaises(subprocess.CalledProcessError):
                    target.active(report, args)
            else:
                target.active(report, args)
        return report, calls

    def test_happy_isolated_ap_and_bluez(self):
        report, calls = self.exercise()
        self.assertTrue(report.channel_36_operation and report.client_association)
        self.assertTrue(report.bluez_profile_registration and report.bluez_agent_registration)
        self.assertIn('save', calls[0])
        self.assertIn('no', calls[0])
        self.assertFalse(report.pairing_asserted or report.trust_asserted)

    def test_ap_failure_cleans_own_uuid(self):
        report, calls = self.exercise(failure=True)
        self.assertEqual(calls[-1][:4], ['nmcli', 'connection', 'delete', 'uuid'])
        self.assertEqual(calls[-1][-1], calls[0][calls[0].index('connection.uuid') + 1])

    def test_unauthorized_station_is_not_client_join(self):
        report, calls = self.exercise(authorized=False)
        self.assertFalse(report.client_association)
        self.assertIn('client-association-not-observed', report.blockers)


class Cli(unittest.TestCase):
    def test_real_cli_refuses_contact_without_window(self):
        result = subprocess.run(['bash', str(ROOT / 'tools/target/probe-wireless.sh'),
                                 '--jetson', 'jetson.local'], capture_output=True, text=True, check=False)
        report = json.loads(result.stdout)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(report['blockers'], ['authorized-inventory-window-required'])
        self.assertEqual(report['schema'], 'aa-wireless-probe/1')
        for field in ('ap_mode', 'channel_36_eligible', 'channel_36_operation', 'client_association',
                      'bluez_profile_registration', 'bluez_agent_registration', 'pairing_asserted',
                      'trust_asserted', 'phone_hosted_ap_or_p2p_asserted', 'target_qualified'):
            self.assertIs(report[field], False)
        self.assertEqual(report['protected_inventory'], {})
        self.assertEqual(report['configured_jurisdiction'], 'US')

    def test_gate_never_invokes_ssh(self):
        with patch.object(sys, 'argv', ['probe', '--jetson', 'jetson.local']), \
                patch.object(host, 'invoke') as invoke, redirect_stdout(io.StringIO()):
            self.assertEqual(host.main(), 1)
        invoke.assert_not_called()


class Window(unittest.TestCase):
    def exercise(self, fail=False, verdict='equivalent', binding=True):
        calls = []
        manifest = {'target_identity': 'jetson.local' if binding else 'other-target', 'task_window_id': 'fixture-window',
                    'roots': [['u', '/home/zucchi/Desktop/infotainment-plan'], ['u', '/approved/codegraph']]}
        def invoke(argv, payload=None):
            calls.append(argv[-1])
            if argv[-1] in ('before', 'after'):
                return '/fixture/' + argv[-1] + '.json\n'
            if fail and argv[-1] == 'true':
                raise OSError('fixture preflight failure')
            return ''
        argv = ['probe', '--jetson', 'jetson.local', '--window-id', 'fixture-window',
                '--window-hook', '/fixture/hook', '--codegraph-root', '/approved/codegraph']
        stdout = io.StringIO()
        observed = asdict(target.Report(target='jetson.local'))
        result = SimpleNamespace(stdout=json.dumps(observed), returncode=0)
        with patch.object(sys, 'argv', argv), patch.object(host, 'invoke', side_effect=invoke), \
                patch.object(host, 'load_manifest', return_value=manifest), \
                patch.object(host, 'diff_manifests', return_value=SimpleNamespace(exit_status=int(verdict != 'equivalent'), verdict=verdict)), \
                patch.object(host.subprocess, 'run', return_value=result), redirect_stdout(stdout):
            exit_code = host.main()
        return exit_code, calls, json.loads(stdout.getvalue())

    def test_closing_sequence_and_references(self):
        code, calls, report = self.exercise()
        self.assertEqual(code, 0)
        self.assertEqual(calls, ['before', 'true', 'workload-cleanup', 'after', 'final-cleanup'])
        self.assertEqual(report['protected_inventory']['verdict'], 'equivalent')

    def test_failure_still_closes_window(self):
        code, calls, report = self.exercise(fail=True)
        self.assertEqual(code, 1)
        self.assertEqual(calls[-3:], ['workload-cleanup', 'after', 'final-cleanup'])

    def test_inventory_difference_is_blocker(self):
        code, calls, report = self.exercise(verdict='different')
        self.assertEqual(code, 1)
        self.assertIn('protected-inventory-not-equivalent', report['blockers'])

    def test_inventory_wrong_target_prevents_preflight(self):
        code, calls, report = self.exercise(binding=False)
        self.assertEqual(code, 1)
        self.assertNotIn('true', calls)


class Bluez(unittest.TestCase):
    def exercise(self, fail_registration=False):
        calls, objects = [], []
        class FakeObject:
            def __init__(self, bus, path):
                objects.append(self)
            def remove_from_connection(self):
                calls.append('remove')
        class DbusError(Exception):
            def get_dbus_name(self):
                return 'org.bluez.Error.Rejected'
        def call(name):
            calls.append(name)
            if fail_registration and name == 'RegisterAgent':
                raise DbusError('fixture registration rejection')
        manager = SimpleNamespace(**{name: (lambda *args, name=name, **kwargs: call(name))
            for name in ('RegisterProfile', 'RegisterAgent', 'UnregisterProfile', 'UnregisterAgent')})
        bus = SimpleNamespace(get_object=lambda *args: manager, close=lambda: calls.append('close'))
        modules = {'dbus': SimpleNamespace(DBusException=DbusError, SystemBus=lambda: bus,
                                          Interface=lambda *args: manager),
                   'dbus.service': SimpleNamespace(Object=FakeObject, method=lambda *args, **kwargs: lambda fn: fn),
                   'dbus.mainloop.glib': SimpleNamespace(DBusGMainLoop=lambda **kwargs: None),
                   'gi.repository.GLib': SimpleNamespace(MainContext=SimpleNamespace(default=lambda: None))}
        report = target.Report()
        with patch.object(bluez.importlib, 'import_module', side_effect=lambda name: modules[name]):
            if fail_registration:
                with self.assertRaises(OSError):
                    with bluez.registration(report):
                        self.fail('rejected agent registration must not yield')
            else:
                with bluez.registration(report):
                    with self.assertRaises(DbusError):
                        objects[1].RequestConfirmation('/fixture/device', 123456)
        return report, calls

    def test_registration_and_rejecting_agent(self):
        report, calls = self.exercise()
        self.assertEqual(calls[:4], ['RegisterProfile', 'RegisterAgent', 'UnregisterAgent', 'UnregisterProfile'])
        self.assertEqual(report.agent_callbacks, ['request-confirmation-rejected'])
        self.assertFalse(report.pairing_asserted or report.trust_asserted)

    def test_partial_registration_failure_cleans_profile(self):
        report, calls = self.exercise(fail_registration=True)
        self.assertIn('bluez-registration-failed', report.blockers)
        self.assertIn('UnregisterProfile', calls)
        self.assertNotIn('UnregisterAgent', calls)


if __name__ == '__main__':
    groups = {'policy': Policy, 'discovery': Discovery, 'active': Active, 'cli': Cli, 'window': Window, 'bluez': Bluez}
    suite = unittest.TestSuite(unittest.defaultTestLoader.loadTestsFromTestCase(group)
        for group in ([groups[sys.argv[1]]] if len(sys.argv) > 1 else groups.values()))
    sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())
