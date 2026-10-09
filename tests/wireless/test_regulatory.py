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

    def test_equivalent_channel_frequencies(self) -> None:
        for frequency in ('5180', '5180.0', '5180.00'):
            with self.subTest(frequency=frequency):
                info = INFO.replace('5180', frequency)
                permitted = target.eligible(info, REG[7:], 'US')
                self.assertTrue(permitted)

    def test_malformed_channel_frequencies_fail_closed(self) -> None:
        for frequency in ('5180.1', '5180.01', '5180.', '.5180', '05180', '+5180',
                          '-5180', '5180e0', '5180,0', '5180.0.0', 'nan', 'inf'):
            with self.subTest(frequency=frequency):
                self.assert_blocked(REG, INFO.replace('5180', frequency))

    def test_cac_milliseconds_and_legacy_forms(self) -> None:
        for cac in ('0 ms', '60000 ms', 'N/A', '0', '60000', '0.0', '-1'):
            with self.subTest(cac=cac):
                reg = REG.replace('(N/A), AUTO-BW', f'({cac}), AUTO-BW')
                permitted = target.eligible(INFO, target.regulatory_section(reg, 'phy0'), 'US')
                self.assertTrue(permitted)

    def test_malformed_cac_units_fail_closed(self) -> None:
        for cac in ('-1 ms', '-0 ms', '+0 ms', '0.0 ms', '60000.5 ms', 'N/A ms',
                    '0 s', '0 us', '0 msec', '0ms', 'ms', '0 ms extra', 'nan ms',
                    '1e3 ms', '0,0 ms'):
            with self.subTest(cac=cac):
                self.assert_blocked(REG.replace('(N/A), AUTO-BW', f'({cac}), AUTO-BW'))
        self.assert_blocked(REG.replace('(N/A, 23)', '(N/A, 23 ms)'))
        self.assert_blocked(REG.replace('(N/A, 23)', '(0 ms, 23)'))

    def test_equivalent_formats_preserve_denials(self) -> None:
        info = INFO.replace('5180', '5180.00')
        reg = REG.replace('(N/A), AUTO-BW', '(60000 ms), AUTO-BW')
        for denied in (reg + reg.splitlines()[-1] + '\n',
                       reg + ' (5180 - 5260 @ 80), (N/A, 23), (0 ms)\n',
                       reg.replace('AUTO-BW', 'AUTO-BW, AUTO_BW'),
                       reg.replace('AUTO-BW', 'DFS'), reg.replace('AUTO-BW', 'UNKNOWN')):
            with self.subTest(reg=denied):
                self.assert_blocked(denied, info)
        for denied in (info + info, info.replace('(23.0 dBm)', '(23.0 dBm) (no IR)'),
                       info.replace('(23.0 dBm)', '(23.0 dBm), UNKNOWN'),
                       info.replace('(23.0 dBm)', '')):
            with self.subTest(info=denied):
                self.assert_blocked(reg, denied)
        for jurisdiction in ('', 'CA'):
            with self.subTest(jurisdiction=jurisdiction):
                self.assertFalse(target.eligible(info, target.regulatory_section(reg, 'phy0'), jurisdiction))

    def test_valid_self_managed_country00_overrides_global(self) -> None:
        reg = REG.replace('(N/A), AUTO-BW', '(0 ms), AUTO-BW')
        reg += REG.replace('global', 'phy#0 (self-managed)').replace('US', '00')
        section = target.regulatory_section(reg, 'phy0')
        self.assertTrue(section.startswith('COUNTRY 00:'))
        self.assert_blocked(reg, INFO.replace('5180', '5180.0'))

    def test_every_section_validated_with_cac_units(self) -> None:
        reg = REG.replace('(N/A), AUTO-BW', '(0 ms), AUTO-BW')
        selected = reg.replace('global', 'phy#0').replace('US', 'CA')
        self.assertTrue(target.eligible(INFO, target.regulatory_section(reg + selected, 'phy0'), 'CA'))
        for other in (reg.replace('global', 'phy#1').replace('0 ms', '-1 ms'),
                      reg.replace('global', 'phy#1') + 'garbage\n'):
            with self.subTest(other=other):
                self.assertEqual(target.regulatory_section(reg + selected + other, 'phy0'), '')

    def test_retained_r2_inputs_remain_blocked(self) -> None:
        # Exact retained regulatory text and channel line; no private device metadata.
        reg = Path(__file__).with_name('retained-r2-iw-reg.txt').read_text(encoding='ascii')
        info = 'Supported interface modes:\n * AP\n* 5180.0 MHz [36] (20.0 dBm)\n'
        for jurisdiction in ('', 'US'):
            with self.subTest(jurisdiction=jurisdiction):
                report = target.Report(confirmed_jurisdiction=jurisdiction)
                with patch.object(target.subprocess, 'run', side_effect=AssertionError('hardware forbidden')), \
                        patch.object(Path, 'resolve', side_effect=[Path('/sys/phy0'), Path('/drivers/rtl88x2ce')]), \
                        patch.object(target, 'command', side_effect=[info, reg, 'GENERAL.STATE:100 (connected)', BUS]):
                    target.discover(report, 'wlan0')
                with patch.object(target.subprocess, 'run', side_effect=AssertionError('hardware forbidden')), \
                        patch.object(target, 'command') as mutation:
                    target.active(report, argparse.Namespace())
                mutation.assert_not_called()
                self.assertIn('rtl8822ce-not-identified', report.blockers)
                self.assertIn('channel-36-ineligible', report.blockers)
                self.assertEqual(report.effective_regulatory_domain, 'unknown')
                self.assertFalse(report.channel_36_operation or report.client_association or report.target_qualified)
                if not jurisdiction:
                    self.assertIn('jurisdiction-unconfirmed', report.blockers)

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
