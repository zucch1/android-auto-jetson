# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tests/wireless/test_psk.py
"""Synthetic credentials only; hook, SSH, and AP commands are mocked."""
import argparse
from contextlib import contextmanager, redirect_stdout
from dataclasses import asdict
import io
import json
import os
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/target'))
import wireless_probe as host
import wireless_target as target

SECRET = 'synthetic-only-credential'
ARGV = ['probe', '--jetson', 'jetson.local', '--active', '--window-id', 'fixture-window',
        '--window-hook', '/fixture/hook', '--codegraph-root', '/approved/codegraph']


class Psk(unittest.TestCase):
    def assert_refused(self, path: Path | None) -> None:
        output = io.StringIO()
        argv = ARGV + (['--ap-psk-file', str(path)] if path else [])
        with patch.object(sys, 'argv', argv), patch.object(host, 'invoke') as invoke, \
                patch.object(host.subprocess, 'run') as run, redirect_stdout(output):
            self.assertEqual(host.main(), 1)
        invoke.assert_not_called()
        run.assert_not_called()
        self.assertIn('private-psk-input-required', json.loads(output.getvalue())['blockers'])
        self.assertNotIn(SECRET, output.getvalue())

    def test_permissions_and_owner_prevent_contact(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'psk'
            path.write_text(SECRET)
            for mode in (0o664, 0o640, 0o604, 0o601, 0o620, 0o666):
                with self.subTest(mode=mode):
                    path.chmod(mode)
                    self.assert_refused(path)
            path.chmod(0o600)
            with patch.object(host.os, 'geteuid', return_value=os.geteuid() + 1):
                self.assert_refused(path)

    def test_invalid_files_and_content_prevent_contact(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'psk'
            for content in (b'', b'short', b'x' * 64, b'\xff' * 8, b'new\nline-password',
                            b'carriage\rreturn', b'outer-space ', b' leading-space', b'password\x00'):
                with self.subTest(content=content):
                    path.write_bytes(content)
                    path.chmod(0o600)
                    self.assert_refused(path)
            self.assert_refused(None)
            self.assert_refused(Path(directory) / 'missing')
            self.assert_refused(Path(directory))
            fifo = Path(directory) / 'fifo'
            os.mkfifo(fifo, 0o600)
            self.assert_refused(fifo)
            path.write_text(SECRET)
            link = Path(directory) / 'link'
            link.symlink_to(path)
            self.assert_refused(link)
            hardlink = Path(directory) / 'hardlink'
            os.link(path, hardlink)
            self.assert_refused(hardlink)

    def test_opened_inode_survives_path_substitution(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'psk'
            path.write_text(SECRET + '\n')
            path.chmod(0o600)
            original = host.os.fstat
            def replace_after_open(fd):
                metadata = original(fd)
                path.rename(Path(directory) / 'opened')
                path.write_text('public-substitution')
                path.chmod(0o666)
                return metadata
            with patch.object(host.os, 'fstat', side_effect=replace_after_open):
                self.assertEqual(host.private_psk(path), SECRET)
            with self.assertRaises(host.InvalidPsk):
                host.private_psk(path)

    def test_host_transfer_argv_and_report_are_secret_free(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'psk'
            path.write_text(SECRET)
            path.chmod(0o600)
            manifest = {'target_identity': 'jetson.local', 'task_window_id': 'fixture-window',
                        'roots': [['u', '/home/zucchi/Desktop/infotainment-plan'], ['u', '/approved/codegraph']]}
            result = SimpleNamespace(stdout=json.dumps(asdict(target.Report(target='jetson.local'))), returncode=0)
            output = io.StringIO()
            payloads = []
            def subprocess_run(argv, **kwargs):
                if argv[0] == 'ssh' and argv[-1] != 'true':
                    payloads.append(kwargs['input'])
                    return result
                return SimpleNamespace(stdout='/fixture/' + argv[-1] + '.json')
            with patch.object(sys, 'argv', ARGV + ['--ap-psk-file', str(path)]), \
                    patch.object(host, 'load_manifest', return_value=manifest), \
                    patch.object(host, 'diff_manifests', return_value=SimpleNamespace(exit_status=0, verdict='equivalent')), \
                    patch.object(host.subprocess, 'run', side_effect=subprocess_run) as run, redirect_stdout(output):
                self.assertEqual(host.main(), 0)
            self.assertIn(SECRET, payloads[0])
            for call in run.call_args_list:
                self.assertNotIn(SECRET, json.dumps(call.args[0]))
            self.assertNotIn(SECRET, output.getvalue())

    def test_nm_secret_stdin_and_failure_diagnostics(self):
        report = target.Report(raw={'nm': 'GENERAL.STATE:30 (disconnected)'})
        args = argparse.Namespace(interface='wlan0', duration=1, ap_psk=SECRET)
        def run(argv, **kwargs):
            if 'up' in argv:
                self.assertEqual(kwargs.get('input'), '802-11-wireless-security.psk:' + SECRET + '\n')
                self.assertEqual(argv[-2:], ['passwd-file', '/dev/stdin'])
                raise subprocess.CalledProcessError(1, argv, stderr=SECRET)
            if 'delete' in argv:
                raise OSError(SECRET)
            return SimpleNamespace(stdout='')
        with patch.object(target.subprocess, 'run', side_effect=run) as commands:
            with self.assertRaises(subprocess.CalledProcessError):
                target.active(report, args)
        for call in commands.call_args_list:
            self.assertNotIn(SECRET, json.dumps(call.args[0]))
        self.assertNotIn(SECRET, json.dumps(asdict(report)))
        self.assertEqual(report.raw['cleanup_error_type'], 'OSError')

    def test_nm_happy_argv_and_report_are_secret_free(self):
        report = target.Report(raw={'nm': 'GENERAL.STATE:30 (disconnected)'})
        args = argparse.Namespace(interface='wlan0', duration=1, ap_psk=SECRET)
        @contextmanager
        def registration(report):
            yield SimpleNamespace(iteration=lambda block: None)
        def run(argv, **kwargs):
            if 'up' in argv:
                self.assertEqual(kwargs.get('input'), '802-11-wireless-security.psk:' + SECRET + '\n')
            if argv[:2] == ['iw', 'dev']:
                return SimpleNamespace(stdout='type AP\nchannel 36 (5180 MHz)' if argv[-1] == 'info' else
                                       'Station aa:bb:cc:dd:ee:ff (on wlan0)\n authorized: yes\n')
            return SimpleNamespace(stdout='')
        import wireless_bluez
        with patch.object(target.subprocess, 'run', side_effect=run) as commands, \
                patch.object(wireless_bluez, 'registration', registration), \
                patch.object(target.time, 'monotonic', side_effect=[0, 0, 2]), patch.object(target.time, 'sleep'):
            target.active(report, args)
        self.assertTrue(report.channel_36_operation and report.client_association)
        for call in commands.call_args_list:
            self.assertNotIn(SECRET, json.dumps(call.args[0]))
        self.assertNotIn(SECRET, json.dumps(asdict(report)))


if __name__ == '__main__':
    unittest.main()
