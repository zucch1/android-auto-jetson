# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Boundary parsing for capability-gate reports: untrusted JSON -> frozen typed
values in capability_model, or a typed GateError (exit 1) for any malformed input
(duplicate JSON keys, invalid UTF-8, overflowing numbers, negative budget
readings, missing identity keys, inconsistent duplicate measured fields)."""
from __future__ import annotations

from datetime import datetime
import json
import math
from pathlib import Path

from capability_budgets import FIXED_BUDGETS, FIXED_BY_ID, Kind, Stage
from capability_model import (
    AREA_IDS,
    PHONE_KEYS,
    SCHEMA,
    SHA256,
    TARGET_KEYS,
    TIMESTAMP,
    AreaView,
    BudgetView,
    Classification,
    Evidence,
    GateError,
    Report,
    RunStage,
    Signoff,
)


def _keys(obj: object, expected: frozenset[str], field: str) -> dict[str, object]:
    if not isinstance(obj, dict):
        raise GateError('schema-type', field)
    if set(obj) != expected:
        raise GateError('schema-keys', field)
    return obj


def _string(value: object, field: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise GateError('schema-type', field)
    return value


def _number(value: object, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, int | float):
        raise GateError('schema-type', field)
    try:
        result = float(value)
    except OverflowError as exc:
        raise GateError('nonfinite', field) from exc
    if not math.isfinite(result):
        raise GateError('nonfinite', field)
    return result


def _nonneg(value: float, field: str) -> float:
    if value < 0:
        raise GateError('budget-negative', field)
    return value


def _flag(value: object, field: str) -> bool:
    if not isinstance(value, bool):
        raise GateError('schema-type', field)
    return value


def _timestamp(value: object, field: str) -> str:
    text = _string(value, field)
    if not TIMESTAMP.match(text):
        raise GateError('signoff-timestamp', field)
    try:
        datetime.strptime(text, '%Y-%m-%dT%H:%M:%SZ')
    except ValueError as exc:
        raise GateError('signoff-timestamp', field) from exc
    return text


def _reject_constant(token: str) -> None:
    raise GateError('nonfinite', token)


def _reject_dup_keys(pairs: list[tuple[str, object]]) -> dict[str, object]:
    seen: dict[str, object] = {}
    for key, value in pairs:
        if key in seen:
            raise GateError('json-duplicate-key', key)
        seen[key] = value
    return seen


def load(path: Path) -> Report:
    try:
        text = path.read_text(encoding='utf-8')
        raw: object = json.loads(text, parse_constant=_reject_constant, object_pairs_hook=_reject_dup_keys)
    except GateError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise GateError('json-parse', str(path)) from exc
    return parse(raw)


def parse(raw: object) -> Report:
    top = _keys(raw, frozenset({'schema', 'generated_at_utc', 'report_stage', 'target', 'phone', 'areas'}), 'report')
    if top['schema'] != SCHEMA:
        raise GateError('schema-type', 'report.schema')
    stage = _string(top['report_stage'], 'report.report_stage')
    if stage not in RunStage.ALL:
        raise GateError('schema-type', 'report.report_stage')
    target = _identity(top['target'], TARGET_KEYS, 'report.target')
    phone = _identity(top['phone'], PHONE_KEYS, 'report.phone')
    areas = _areas(top['areas'])
    _fixed_grid(areas)
    return Report(
        generated_at_utc=_timestamp(top['generated_at_utc'], 'report.generated_at_utc'),
        report_stage=stage,
        target=target,
        phone=phone,
        areas=areas,
    )


def _identity(obj: object, expected: frozenset[str], field: str) -> dict[str, str]:
    entry = _keys(obj, expected, field)
    return {str(key): _string(value, f'{field}.{key}') for key, value in entry.items()}


def _areas(obj: object) -> tuple[AreaView, ...]:
    if not isinstance(obj, list):
        raise GateError('schema-type', 'report.areas')
    parsed = tuple(_area(entry, index) for index, entry in enumerate(obj))
    seen: set[str] = set()
    for area in parsed:
        if area.id in seen:
            raise GateError('area-duplicate', f'report.areas.{area.id}')
        seen.add(area.id)
    for area_id in AREA_IDS:
        if area_id not in seen:
            raise GateError('area-missing', f'report.areas.{area_id}')
    for area_id in sorted(seen - set(AREA_IDS)):
        raise GateError('area-unknown', f'report.areas.{area_id}')
    return parsed


def _area(obj: object, index: int) -> AreaView:
    field = f'report.areas[{index}]'
    entry = _keys(obj, frozenset({'id', 'classification', 'summary', 'evidence', 'measurements', 'budgets', 'owner_signoff'}), field)
    area_id = _string(entry['id'], f'{field}.id')
    classification = _string(entry['classification'], f'{field}.classification')
    if classification not in Classification.ALL:
        raise GateError('classification-invalid', f'{field}.classification')
    signoff_raw = entry['owner_signoff']
    if classification != Classification.OWNER_SIGNED_REDUCED_SCOPE:
        if signoff_raw is not None:
            raise GateError('signoff-unexpected', f'{field}.owner_signoff')
    elif signoff_raw is None:
        raise GateError('signoff-missing', f'{field}.owner_signoff')
    signoff = _signoff(signoff_raw, f'{field}.owner_signoff') if signoff_raw is not None else None
    measurements = _measurements(entry['measurements'], f'{field}.measurements')
    budgets = _budgets(entry['budgets'], f'{field}.budgets', classification, signoff)
    _consistent(measurements, budgets, field)
    return AreaView(
        id=area_id,
        classification=classification,
        summary=_string(entry['summary'], f'{field}.summary'),
        evidence=_evidence(entry['evidence'], f'{field}.evidence'),
        measurements=measurements,
        budgets=budgets,
        signoff=signoff,
    )


def _evidence(obj: object, field: str) -> tuple[Evidence, ...]:
    if not isinstance(obj, list) or not obj:
        raise GateError('evidence-invalid', field)
    out: list[Evidence] = []
    for index, item in enumerate(obj):
        entry = _keys(item, frozenset({'name', 'sha256'}), f'{field}[{index}]')
        digest = _string(entry['sha256'], f'{field}[{index}].sha256')
        if not SHA256.match(digest):
            raise GateError('evidence-invalid', f'{field}[{index}].sha256')
        out.append(Evidence(name=_string(entry['name'], f'{field}[{index}].name'), sha256=digest))
    return tuple(out)


def _measurements(obj: object, field: str) -> dict[str, str | int | float | bool]:
    if not isinstance(obj, dict) or not obj:
        raise GateError('schema-type', field)
    out: dict[str, str | int | float | bool] = {}
    for key, value in obj.items():
        cell = f'{field}.{key}'
        if isinstance(value, bool):
            out[str(key)] = value
        elif isinstance(value, int | float):
            out[str(key)] = _number(value, cell)
        elif isinstance(value, str):
            out[str(key)] = _string(value, cell)
        else:
            raise GateError('schema-type', cell)
    return out


def _signoff(obj: object, field: str) -> Signoff:
    entry = _keys(obj, frozenset({'owner', 'statement', 'withdrawn_claims', 'evidence_ref', 'timestamp'}), field)
    claims = entry['withdrawn_claims']
    if not isinstance(claims, list) or not claims:
        raise GateError('signoff-withdrawn-claims', f'{field}.withdrawn_claims')
    parsed = tuple(_string(claim, f'{field}.withdrawn_claims[{i}]') for i, claim in enumerate(claims))
    if len(set(parsed)) != len(parsed):
        raise GateError('signoff-withdrawn-claims', f'{field}.withdrawn_claims')
    return Signoff(
        owner=_string(entry['owner'], f'{field}.owner'),
        statement=_string(entry['statement'], f'{field}.statement'),
        withdrawn_claims=parsed,
        evidence_ref=_string(entry['evidence_ref'], f'{field}.evidence_ref'),
        timestamp=_timestamp(entry['timestamp'], f'{field}.timestamp'),
    )


def _budgets(obj: object, field: str, classification: str, signoff: Signoff | None) -> tuple[BudgetView, ...]:
    if not isinstance(obj, list):
        raise GateError('schema-type', field)
    claims = signoff.withdrawn_claims if signoff is not None else ()
    out: list[BudgetView] = []
    for index, item in enumerate(obj):
        cell = f'{field}[{index}]'
        entry = _keys(item, frozenset({'id', 'kind', 'limit', 'unit', 'available_at', 'measured', 'passed', 'withdrawn', 'withdrawn_claim'}), cell)
        budget_id = _string(entry['id'], f'{cell}.id')
        fixed = FIXED_BY_ID.get(budget_id)
        if fixed is None:
            raise GateError('budget-unknown', f'{cell}.id')
        kind = _string(entry['kind'], f'{cell}.kind')
        limit = _nonneg(_number(entry['limit'], f'{cell}.limit'), f'{cell}.limit')
        unit = _string(entry['unit'], f'{cell}.unit')
        stage = _string(entry['available_at'], f'{cell}.available_at')
        if stage not in {str(member) for member in Stage}:
            raise GateError('budget-mismatch', f'{cell}.available_at')
        if kind != fixed.kind or limit != fixed.limit:
            raise GateError('budget-weakened', f'{cell}.limit')
        if unit != fixed.unit or stage != fixed.available_at:
            raise GateError('budget-mismatch', f'{cell}.unit')
        measured_raw = entry['measured']
        measured = None if measured_raw is None else _nonneg(_number(measured_raw, f'{cell}.measured'), f'{cell}.measured')
        withdrawn = _flag(entry['withdrawn'], f'{cell}.withdrawn')
        claim_raw = entry['withdrawn_claim']
        claim = None if claim_raw is None else _string(claim_raw, f'{cell}.withdrawn_claim')
        if withdrawn:
            if classification != Classification.OWNER_SIGNED_REDUCED_SCOPE:
                raise GateError('withdrawn-unexpected', f'{cell}.withdrawn')
            if claim is None or claim not in claims:
                raise GateError('withdrawn-claim-unlisted', f'{cell}.withdrawn_claim')
        elif claim is not None:
            raise GateError('withdrawn-claim-unexpected', f'{cell}.withdrawn_claim')
        out.append(BudgetView(
            id=budget_id, kind=Kind(kind), limit=limit, unit=unit, available_at=Stage(stage),
            measured=measured, passed=_flag(entry['passed'], f'{cell}.passed'),
            withdrawn=withdrawn, withdrawn_claim=claim,
        ))
    return tuple(out)


def _consistent(measurements: dict[str, str | int | float | bool], budgets: tuple[BudgetView, ...], field: str) -> None:
    # A quantity recorded both as a measurement and a budget must agree exactly.
    for budget in budgets:
        if budget.measured is None:
            continue
        key = budget.id.split('.', 1)[1] if '.' in budget.id else budget.id
        value = measurements.get(key)
        if isinstance(value, bool) or not isinstance(value, int | float):
            continue
        if float(value) != budget.measured:
            raise GateError('measured-inconsistent', f'{field}.{budget.id}')


def _fixed_grid(areas: tuple[AreaView, ...]) -> None:
    seen: dict[str, str] = {}
    for area in areas:
        for budget in area.budgets:
            if budget.id in seen:
                raise GateError('budget-duplicate', f'{area.id}.budgets.{budget.id}')
            seen[budget.id] = area.id
    for fixed in FIXED_BUDGETS:
        owner = seen.get(fixed.id)
        if owner is None:
            raise GateError('budget-missing', f'{fixed.area}.budgets.{fixed.id}')
        if owner != fixed.area:
            raise GateError('budget-mismatch', f'{fixed.area}.budgets.{fixed.id}')
