# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent privacy controls use only invented identifiers and capture bytes."""
from __future__ import annotations

import json
import unittest

from fixture import WIRELESS_PRIVACY_CONTROLS, PackageTest, binding, rebind, write
from common import load


class PrivacyTests(PackageTest):
    def test_duplicate_json_keys_rejected_without_echoing_values(self) -> None:
        # Given: retained raw identifiers hidden by duplicate keys in rehashed artifacts.
        controls = (
            '{"phone_id":"TESTONLY_RAW_ID","phone_id":"[REDACTED]"}',
            '{"events":[{"phone_id":"TESTONLY_RAW_ID",'
            '"phone_id":"[REDACTED]"}]}',
            r'{"phone_id":"TESTONLY_RAW_ID","phone\u005fid":"[REDACTED]"}',
            '{"event":{"phone_id":"TESTONLY_RAW_ID"},'
            '"event":{"phone_id":"[REDACTED]"}}',
        )
        for suffix in ('.json', '.jsonl', '.ndjson'):
            path = self.root / ('duplicate' + suffix)
            for index, content in enumerate(controls):
                path.write_text(content + '\n', encoding='utf-8')
                doc = load(self.manifest)
                doc.values['session_trace'] = binding(path)
                write(self.manifest, doc.values)
                # When: both shipped CLIs inspect the package with matching artifact hashes.
                for tool in ('check_privacy.py', 'check_wired.py'):
                    with self.subTest(suffix=suffix, control=index, tool=tool):
                        result = self.cli(tool)
                        # Then: duplicates fail closed without exposing the retained raw value.
                        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                        self.assertNotIn('TESTONLY_RAW_ID', result.stdout + result.stderr)
            path.unlink()

    def test_canonical_identifiers_rejected_when_raw_on_both_clis(self) -> None:
        # Given: canonical native identifier keys with invented raw values.
        names = ('phone_id', 'identity', 'device_id', 'uuid', 'subscriber_id',
                 'meid', 'udid', 'guid', 'phone-id', 'device id', 'subscriberid')
        for name in names:
            for filename, content in (
                ('sanitized.log', f'{name}=TESTONLY_RAW_ID'),
                ('session.jsonl', json.dumps({name: 'TESTONLY_RAW_ID'})),
            ):
                with self.subTest(name=name, filename=filename):
                    (self.root / filename).write_text(content, encoding='utf-8')
                    rebind(self.manifest, 'redacted_log' if filename.endswith('.log')
                           else 'session_trace')
                    # When: either shipped CLI inspects the rehashed export.
                    for tool in ('check_privacy.py', 'check_wired.py'):
                        result = self.cli(tool)
                        # Then: rejection withholds even partial matched content.
                        self.assertEqual(result.returncode, 1, result.stdout)
                        self.assertNotIn('TESTONLY_RAW_ID', result.stdout + result.stderr)
                    (self.root / filename).write_text('null\n', encoding='utf-8')
                    rebind(self.manifest, 'redacted_log' if filename.endswith('.log')
                           else 'session_trace')

    def test_safe_prefix_rejected_when_value_has_raw_tail(self) -> None:
        # Given: safe-looking prefixes followed by invented private content.
        values = ('[REDACTED] TESTONLY_RAW_TAIL', 'redacted TESTONLY_RAW_TAIL',
                  'null TESTONLY_RAW_TAIL', 'sha256:' + 'a' * 16 + ' TESTONLY_RAW_TAIL',
                  'id-' + 'a' * 16 + ' TESTONLY_RAW_TAIL',
                  '[REDACTED],TESTONLY_RAW_TAIL', '[REDACTED]"TESTONLY_RAW_TAIL')
        for value in values:
            contents = (('sanitized.log', f'token={value}'),
                        ('sanitized.log', f'token={value!r}'),
                        ('sanitized.log', json.dumps({'token': value})),
                        ('session.jsonl', json.dumps({'token': value})))
            for index, (filename, content) in enumerate(contents):
                with self.subTest(value=values.index(value), form=index):
                    (self.root / 'sanitized.log').write_text('safe\n', encoding='utf-8')
                    (self.root / 'session.jsonl').write_text('null\n', encoding='utf-8')
                    (self.root / filename).write_text(content, encoding='utf-8')
                    rebind(self.manifest, 'redacted_log')
                    rebind(self.manifest, 'session_trace')
                    # When: both CLI surfaces inspect the entire assignment.
                    for tool in ('check_privacy.py', 'check_wired.py'):
                        result = self.cli(tool)
                        # Then: neither a safe prefix nor quoting conceals the tail.
                        self.assertEqual(result.returncode, 1, result.stdout)
                        self.assertNotIn('TESTONLY_RAW_TAIL', result.stdout + result.stderr)

    def test_native_pseudonym_rejected_when_shape_is_invalid(self) -> None:
        # Given: near-miss native labels must not become a general id- allowlist.
        for value in ('id-0123456789abcde', 'id-0123456789abcdef0',
                      'id-0123456789abcdeg', 'id-0123456789ABCDEF'):
            with self.subTest(value=value):
                (self.root / 'sanitized.log').write_text(f'phone_id={value}\n',
                                                        encoding='utf-8')
                rebind(self.manifest, 'redacted_log')
                # When: either CLI checks the identifier assignment.
                results = [self.cli(tool) for tool in ('check_privacy.py', 'check_wired.py')]
                # Then: only the exact native lowercase 16-hex format is safe.
                for result in results:
                    self.assertEqual(result.returncode, 1, result.stdout)
                    self.assertNotIn(value, result.stdout + result.stderr)

    def test_raw_identifiers_fail_both_clis_without_echoing_values(self) -> None:
        # Given: independently injected, rehashed synthetic private values.
        controls = ('usb_serial=TESTONLY12345', 'SerialNumber: TESTONLY12345',
                    'Serial Number: TESTONLY12345',
                    'iSerial 3 TESTONLY12345', 'private_key=TESTONLY_CREDENTIAL',
                    'access_token=TESTONLY_CREDENTIAL',
                    '{"iSerial":"TESTONLY12345"}', 'mac=02:11:22:33:44:55',
                    'latitude=12.345678', 'longitude=-123.456789',
                    'location=TEST_ONLY_PLACE', 'password=TESTONLY_CREDENTIAL',
                    'Authorization: Bearer TESTONLY_CREDENTIAL',
                    '-----BEGIN ' + 'PRIVATE KEY-----',
                    'https://test:TESTONLY_CREDENTIAL@example.invalid',
                    '{"mac":"02\\u003a11\\u003a22\\u003a33\\u003a44\\u003a55"}')
        for value in controls:
            with self.subTest(control=controls.index(value)):
                path = self.root / 'sanitized.log'
                path.write_text(value, encoding='utf-8')
                rebind(self.manifest, 'redacted_log')
                # When: both CLI surfaces inspect the package.
                results = [self.cli(tool) for tool in ('check_privacy.py', 'check_wired.py')]
                # Then: no private matched content appears in diagnostics.
                for result in results:
                    self.assertEqual(result.returncode, 1, result.stdout)
                    self.assertNotIn(value, result.stdout + result.stderr)

    def test_wireless_stack_output_fails_both_clis(self) -> None:
        path = self.root / 'sanitized.log'
        for index, (control, marker) in enumerate(WIRELESS_PRIVACY_CONTROLS):
            with self.subTest(format=index):
                path.write_text(control + '\n', encoding='utf-8')
                rebind(self.manifest, 'redacted_log')
                for tool in ('check_privacy.py', 'check_wired.py'):
                    result = self.cli(tool)
                    self.assertEqual(result.returncode, 1)
                    self.assertNotIn(marker, result.stdout + result.stderr)

    def test_wireless_identifiers_in_filenames_are_rejected(self) -> None:
        for name in ('receipt_dev_02_11_22_33_44_55.log', 'receipt_AA-0123456789abcdef.log'):
            path = self.root / name
            path.write_text('Synthetic sanitized output', encoding='utf-8')
            result = self.cli('check_privacy.py')
            self.assertEqual(result.returncode, 1)
            self.assertNotIn(name, result.stdout + result.stderr)
            path.unlink()

    def test_decoded_wireless_log_lines_are_checked(self) -> None:
        path = self.root / 'session.jsonl'
        for control in ('\tssid TESTONLY_RAW', '[ro.serialno]: [TESTONLY_RAW]',
                        'wpa_psk=TESTONLY_RAW'):
            write(path, {'nested': [{'log': 'header\n' + control + '\nfooter'}]})
            rebind(self.manifest, 'session_trace')
            result = self.cli('check_privacy.py')
            self.assertEqual(result.returncode, 1)
            self.assertNotIn('TESTONLY_RAW', result.stdout + result.stderr)

        write(path, {'nested': [{'log': '\tssid [REDACTED]\n[ro.serialno]: [[REDACTED]]\n'
                                    'wpa_psk=[REDACTED]'}]})
        rebind(self.manifest, 'session_trace')
        self.assertEqual(self.cli('check_privacy.py').returncode, 0)

    def test_raw_material_after_escaped_whitespace_is_checked(self) -> None:
        path = self.root / 'session.jsonl'
        for control in ('AA-0123456789abcdef', '02:00:5e:00:53:01',
                        'iSerial 3 TESTONLY0RAW', 'Bearer TESTONLYRAWTOKEN',
                        '12.345678, -98.765432'):
            for separator in ('\n', '\t'):
                write(path, {'nested': [{'log': 'header' + separator + control}]})
                rebind(self.manifest, 'session_trace')
                result = self.cli('check_privacy.py')
                self.assertEqual(result.returncode, 1, (control, separator))
                self.assertNotIn(control, result.stdout + result.stderr)

    def test_unreferenced_capture_files_fail(self) -> None:
        # Given: raw material must fail even outside manifest references.
        for filename, data in (('traffic.pcap', b'fake capture'),
                               ('02:11:22:33:44:55.log', b'sanitized text'),
                               ('innocent.txt', b'\xd4\xc3\xb2\xa1fake'),
                               ('innocent.log', b'aa-projection-perf-capture/1'),
                               ('phone.key', b'fake private key'),
                               ('unknown.dat', b'opaque material')):
            with self.subTest(filename=filename):
                path = self.root / filename
                path.write_bytes(data)
                # When: the standalone privacy CLI scans the entire directory.
                result = self.cli('check_privacy.py')
                # Then: raw/opaque material is rejected; file removed for next control.
                self.assertEqual(result.returncode, 1)
                path.unlink()

    def test_documented_redaction_forms_pass(self) -> None:
        # Given: explicit redaction and one-way pseudonym values.
        (self.root / 'sanitized.log').write_text(
            'usb_serial=[REDACTED]\nmac=sha256:' + 'a' * 64 + '\npassword=redacted\n')
        rebind(self.manifest, 'redacted_log')
        # When: both CLI surfaces inspect the export.
        results = [self.cli(tool) for tool in ('check_privacy.py', 'check_wired.py')]
        # Then: recognized sanitized forms are permitted.
        for result in results:
            self.assertEqual(result.returncode, 0, result.stdout)

    def test_native_pseudonyms_and_safe_forms_pass_both_clis(self) -> None:
        # Given: exact native pseudonyms and all existing safe assignment forms.
        names = ('phone_id', 'identity', 'device_id', 'uuid', 'subscriber_id',
                 'meid', 'udid', 'guid', 'token')
        for value in ('id-0123456789abcdef', '[REDACTED]', 'redacted', 'null',
                      'sha256:' + 'a' * 16, 'sha256:' + 'b' * 64):
            with self.subTest(value=value):
                (self.root / 'sanitized.log').write_text(
                    '\n'.join(f'{name}={value}\n{name}=\'{value}\'' for name in names),
                    encoding='utf-8')
                (self.root / 'session.jsonl').write_text(
                    json.dumps(dict.fromkeys(names, value)) + '\n', encoding='utf-8')
                rebind(self.manifest, 'redacted_log')
                rebind(self.manifest, 'session_trace')
                # When: both shipped CLIs inspect text and JSON assignments.
                results = [self.cli(tool) for tool in ('check_privacy.py', 'check_wired.py')]
                # Then: legitimate sanitization remains exportable.
                for result in results:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
