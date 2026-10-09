# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wireless/test_regulatory.py
"""Discovery-to-active regressions; every hardware command is mocked."""
import argparse
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/target'))
import wireless_target as target

INFO = 'Supported interface modes:\n * AP\n * 5180 MHz [36] (23.0 dBm)\n'
REG = 'global\ncountry US: DFS-FCC\n (5170 - 5250 @ 80), (N/A, 23), (N/A), AUTO-BW\n'
BUS = 'org.bluez.ProfileManager1\norg.bluez.AgentManager1\n'


class Regulatory(unittest.TestCase):
    def assert_blocked(self, reg: str, info: str = INFO) -> None:
        report = target.Report(confirmed_jurisdiction='US')
        with patch.object(Path, 'resolve', side_effect=[Path('/sys/phy0'), Path('/drivers/rtw_8822ce')]), \
                patch.object(target, 'command', side_effect=[info, reg, 'GENERAL.STATE:30 (disconnected)', BUS]):
            target.discover(report, 'wlan0')
        with patch.object(target, 'command', return_value='') as mutation:
            target.active(report, argparse.Namespace(interface='wlan0', ap_psk='synthetic-only-secret', duration=1))
        mutation.assert_not_called()
        self.assertIn('channel-36-ineligible', report.blockers)

    def test_sections_fail_closed(self):
        restricted = REG.replace('global', 'phy#0 (self-managed)').replace('US', '00').replace('AUTO-BW', 'NO-IR')
        for reg in (REG + ' ' + restricted, REG + restricted.upper(), REG + REG,
                    REG + restricted + restricted, REG + 'phy#0 nonsense\n',
                    REG + restricted.replace('phy#0', 'phy#00'), REG.replace('US', 'U\u017f'),
                    REG + 'phy#0 (self-managed)\n', 'garbage\n' + REG):
            with self.subTest(reg=reg):
                self.assert_blocked(reg)

    def test_countries_and_rules_fail_closed(self):
        for reg in (REG + 'country 00: DFS-UNSET\n', REG + 'country US: DFS-FCC\n',
                    REG.replace('country US:', 'country USA:'), REG.replace('country US:', 'country us :'),
                    REG.replace('(5170 - 5250 @ 80), (N/A, 23), (N/A), AUTO-BW', 'garbage (5170 - 5250 @ 80) garbage'),
                    REG + REG.splitlines()[-1] + '\n', REG + ' (5180 - 5260 @ 80), (N/A, 23), (N/A)\n',
                    REG.replace('5170 - 5250', '5250 - 5170'), REG.replace('@ 80', '@ 0'),
                    REG.replace('AUTO-BW', 'AUTO-BW, AUTO_BW'), REG + 'garbage\n'):
            with self.subTest(reg=reg):
                self.assert_blocked(reg)

    def test_restrictions_and_unknown_channels_fail_closed(self):
        for flag in ('no\tIR', 'no  IR', 'NO_IR', 'no-ir', 'radar detection', 'unknown',
                     'no  20MHz', 'no\tOFDM', '', '(garbage dBm)', '(nan dBm)', '(23.0 dBm), unknown'):
            with self.subTest(flag=flag):
                self.assert_blocked(REG, INFO.replace('(23.0 dBm)', flag))
        for flag in ('NO\tIR', 'NO  IR', 'NO_IR', 'NO-IR', 'DFS', 'UNKNOWN', 'NO  OUTDOOR'):
            with self.subTest(flag=flag):
                self.assert_blocked(REG.replace('AUTO-BW', flag))

    def test_canonical_confirmed_us_reaches_active(self):
        report = target.Report(confirmed_jurisdiction='US')
        with patch.object(Path, 'resolve', side_effect=[Path('/sys/phy0'), Path('/drivers/rtw_8822ce')]), \
                patch.object(target, 'command', side_effect=[INFO, REG, 'GENERAL.STATE:30 (disconnected)', BUS]):
            target.discover(report, 'wlan0')
        self.assertEqual(report.blockers, [])
        self.assertEqual(report.effective_regulatory_domain, 'US')
        args = argparse.Namespace(interface='wlan0', ap_psk='synthetic-only-secret', duration=1)
        with patch.object(target, 'command', side_effect=OSError('synthetic stop before hardware')) as mutation:
            with self.assertRaises(OSError):
                target.active(report, args)
        self.assertEqual(mutation.call_args_list[0].args[0][:3], ['nmcli', 'connection', 'add'])


if __name__ == '__main__':
    unittest.main()
