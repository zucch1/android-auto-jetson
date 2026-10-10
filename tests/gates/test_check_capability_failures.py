# SPDX-License-Identifier: GPL-3.0-or-later
"""Failure-mode tests for the capability gate CLI."""
from __future__ import annotations

import copy
import json

from support import FIXTURES, GateCase


class FailureCases(GateCase):
    """Failure modes: every case must exit 1 with a stable error code."""

    def setUp(self) -> None:
        self.base = json.loads((FIXTURES / 'good_provisional.json').read_text(encoding='utf-8'))

    def test_missing_decode_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        data['areas'] = [a for a in data['areas'] if a['id'] != 'decode']
        self.assert_exit(data, 1, 'area-missing')

    def test_missing_phone_fields_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        data['phone'] = {'source': 'owner-reported'}
        self.assert_exit(data, 1, 'schema-keys')

    def test_missing_target_field_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        data['target'] = {'architecture': 'aarch64'}
        self.assert_exit(data, 1, 'schema-keys')

    def test_signoff_missing_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        for area in data['areas']:
            if area['id'] == 'decode':
                area['classification'] = 'owner-signed-reduced-scope'
                area['owner_signoff'] = None
        self.assert_exit(data, 1, 'signoff-missing')

    def test_signoff_empty_claims_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        for area in data['areas']:
            if area['id'] == 'decode':
                area['classification'] = 'owner-signed-reduced-scope'
                area['owner_signoff'] = {
                    'owner': 'Newton', 'statement': 'reduced scope',
                    'withdrawn_claims': [],
                    'evidence_ref': '.omo/evidence/x.json',
                    'timestamp': '2026-10-06T15:57:52Z',
                }
        self.assert_exit(data, 1, 'signoff-withdrawn-claims')

    def test_signoff_bad_timestamp_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        for area in data['areas']:
            if area['id'] == 'decode':
                area['classification'] = 'owner-signed-reduced-scope'
                area['owner_signoff'] = {
                    'owner': 'Newton', 'statement': 'reduced scope',
                    'withdrawn_claims': ['claim one'],
                    'evidence_ref': '.omo/evidence/x.json',
                    'timestamp': 'not-a-timestamp',
                }
        self.assert_exit(data, 1, 'signoff-timestamp')

    def test_release_stage_failure_exit1(self) -> None:
        self.assert_exit(self.base, 1, 'provisional-unresolved', stage='final-release')

    def test_budget_weakening_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['limit'] = 40.0
        self.assert_exit(data, 1, 'budget-weakened')

    def test_budget_kind_weakening_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['kind'] = 'min'
        self.assert_exit(data, 1, 'budget-weakened')

    def test_budget_omitted_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        area = self.mutate(data, 'decode')
        area['budgets'] = [b for b in area['budgets'] if b['id'] != 'decode.p95_latency_ms']
        self.assert_exit(data, 1, 'budget-missing')

    def test_bad_numeric_string_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['measured'] = '30.0'
        self.assert_exit(data, 1, 'schema-type')

    def test_bad_numeric_bool_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['limit'] = True
        self.assert_exit(data, 1, 'schema-type')

    def test_nonfinite_exit1(self) -> None:
        raw = (FIXTURES / 'good_provisional.json').read_text(encoding='utf-8')
        broken = raw.replace('"measured": 30.0', '"measured": NaN', 1)
        self.assert_raw_exit(broken.encode(), 1, 'nonfinite')

    def test_huge_int_measured_exit1(self) -> None:
        raw = (FIXTURES / 'good_provisional.json').read_text(encoding='utf-8')
        broken = raw.replace('"measured": 30.0', '"measured": ' + '9' * 400, 1)
        self.assert_raw_exit(broken.encode(), 1, 'nonfinite')

    def test_invalid_utf8_exit1(self) -> None:
        raw = (FIXTURES / 'good_provisional.json').read_bytes()
        self.assert_raw_exit(b'\xff\xfe' + raw, 1, 'json-parse')

    def test_duplicate_json_key_exit1(self) -> None:
        raw = (FIXTURES / 'good_provisional.json').read_text(encoding='utf-8')
        broken = raw.replace('"report_stage": "continuation",',
                             '"report_stage": "continuation", "report_stage": "continuation",', 1)
        self.assert_raw_exit(broken.encode(), 1, 'json-duplicate-key')

    def test_negative_measured_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        self.mutate(data, 'decode', 'audio.path_latency_ms')['measured'] = -1.0
        self.assert_exit(data, 1, 'budget-negative')

    def test_inconsistent_duplicate_measured_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        self.mutate(data, 'decode')['measurements']['p95_latency_ms'] = 30.0
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['measured'] = 36.09
        budget['passed'] = False
        self.assert_exit(data, 1, 'measured-inconsistent')

    def test_false_full_pass_unmeasured_release_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        self.mutate(data, 'usb')['classification'] = 'full-pass'
        self.assert_exit(data, 1, 'budget-unmeasured')

    def test_false_passed_unmeasured_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        budget = self.mutate(data, 'decode', 'audio.path_latency_ms')
        budget['passed'] = True
        self.assert_exit(data, 1, 'budget-unmeasured-pass')

    def test_verdict_mismatch_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        self.mutate(data, 'decode')['measurements']['p95_latency_ms'] = 36.09
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['measured'] = 36.09
        budget['passed'] = True
        self.assert_exit(data, 1, 'budget-verdict-mismatch')

    def test_unsigned_budget_fail_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        self.mutate(data, 'decode')['measurements']['p95_latency_ms'] = 36.09
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['measured'] = 36.09
        budget['passed'] = False
        self.assert_exit(data, 1, 'budget-fail-unsigned')

    def test_withdrawn_unlisted_claim_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        area = self.mutate(data, 'decode')
        area['classification'] = 'owner-signed-reduced-scope'
        area['owner_signoff'] = {
            'owner': 'Newton', 'statement': 'reduced scope',
            'withdrawn_claims': ['some other claim'],
            'evidence_ref': '.omo/evidence/x.json',
            'timestamp': '2026-10-06T15:57:52Z',
        }
        area['measurements']['p95_latency_ms'] = 36.09
        budget = self.mutate(data, 'decode', 'decode.p95_latency_ms')
        budget['measured'] = 36.09
        budget['passed'] = False
        budget['withdrawn'] = True
        budget['withdrawn_claim'] = 'decode p95 latency at most 33 ms at 1280x720@30'
        self.assert_exit(data, 1, 'withdrawn-claim-unlisted')

    def test_blocker_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        area = self.mutate(data, 'usb')
        area['classification'] = 'blocker'
        self.assert_exit(data, 1, 'blocker-present')

    def test_invalid_evidence_digest_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        area = self.mutate(data, 'dependencies')
        area['evidence'][0]['sha256'] = 'not-a-digest'
        self.assert_exit(data, 1, 'evidence-invalid')

    def test_empty_evidence_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        area = self.mutate(data, 'dependencies')
        area['evidence'] = []
        self.assert_exit(data, 1, 'evidence-invalid')

    def test_unexpected_signoff_exit1(self) -> None:
        data = copy.deepcopy(self.base)
        area = self.mutate(data, 'usb')
        area['owner_signoff'] = {
            'owner': 'Newton', 'statement': 'stray sign-off',
            'withdrawn_claims': ['claim'],
            'evidence_ref': '.omo/evidence/x.json',
            'timestamp': '2026-10-08T17:28:33Z',
        }
        self.assert_exit(data, 1, 'signoff-unexpected')
