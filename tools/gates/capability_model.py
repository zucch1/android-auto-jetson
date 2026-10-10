# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Typed value objects and schema constants for capability-gate reports.

The boundary parser (capability_parse) turns untrusted report JSON into these
frozen typed values. Numbers must be real finite numbers (never bool, string or
NaN/Infinity), flags must be real bools, and the target/phone identity blocks
must carry their required keys exactly.
"""
from __future__ import annotations

from dataclasses import dataclass
import re
from typing import Final

from capability_budgets import Kind, Stage

SCHEMA: Final = 'aa-capability-gate/1'
AREA_IDS: Final = ('dependencies', 'target_identity', 'abi', 'tls', 'decode', 'ipc', 'usb', 'wireless')
TARGET_KEYS: Final = frozenset({'architecture', 'l4t_release', 'kernel_release', 'gstreamer_version'})
PHONE_KEYS: Final = frozenset({'model', 'android_version', 'android_auto_version', 'source'})
TIMESTAMP: Final = re.compile(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$')
SHA256: Final = re.compile(r'^[0-9a-f]{64}$')


class Classification:
    FULL_PASS = 'full-pass'
    PROBE_PASS_PROVISIONAL = 'probe-pass-provisional'
    FALLBACK_PROBE_PASS = 'fallback-probe-pass'
    OWNER_SIGNED_REDUCED_SCOPE = 'owner-signed-reduced-scope'
    BLOCKER = 'blocker'
    ALL: Final = (FULL_PASS, PROBE_PASS_PROVISIONAL, FALLBACK_PROBE_PASS, OWNER_SIGNED_REDUCED_SCOPE, BLOCKER)
    INTERIM: Final = (PROBE_PASS_PROVISIONAL, FALLBACK_PROBE_PASS)


class RunStage:
    CONTINUATION = 'continuation'
    FINAL_RELEASE = 'final-release'
    ALL: Final = (CONTINUATION, FINAL_RELEASE)


@dataclass(frozen=True, slots=True)
class GateError(Exception):
    code: str
    field: str

    def __str__(self) -> str:
        return f'{self.code}\t{self.field}'


@dataclass(frozen=True, slots=True)
class Evidence:
    name: str
    sha256: str


@dataclass(frozen=True, slots=True)
class Signoff:
    owner: str
    statement: str
    withdrawn_claims: tuple[str, ...]
    evidence_ref: str
    timestamp: str


@dataclass(frozen=True, slots=True)
class BudgetView:
    id: str
    kind: Kind
    limit: float
    unit: str
    available_at: Stage
    measured: float | None
    passed: bool
    withdrawn: bool
    withdrawn_claim: str | None


@dataclass(frozen=True, slots=True)
class AreaView:
    id: str
    classification: str
    summary: str
    evidence: tuple[Evidence, ...]
    measurements: dict[str, str | int | float | bool]
    budgets: tuple[BudgetView, ...]
    signoff: Signoff | None


@dataclass(frozen=True, slots=True)
class Report:
    generated_at_utc: str
    report_stage: str
    target: dict[str, str]
    phone: dict[str, str]
    areas: tuple[AreaView, ...]
