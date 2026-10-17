#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Strict, fail-closed JSON Schema validator for the task-32 perf document.

Stdlib only. This implements the documented subset of JSON Schema
(draft 2020-12) that ``aa-projection-perf.schema.json`` uses, so the schema
file is the single source of truth for the result-document contract and the
tool can validate its own output before it returns an exit code.

Supported keywords: ``$schema`` / ``$id`` / ``$defs`` / ``$ref`` (local
``#/...`` JSON pointers only), ``title`` / ``description`` (annotations),
``type`` (string or list), ``required``, ``properties``, ``additionalProperties``
(boolean or schema), ``enum``, ``const``, ``minimum``, ``maximum``,
``exclusiveMinimum``, ``exclusiveMaximum``, ``minItems``, ``maxItems``,
``uniqueItems``, ``items``, ``minLength``, ``maxLength``, ``pattern``,
``format`` (``date-time`` only), ``allOf``, ``anyOf``, ``oneOf`` and ``not``.

Anything else is *not* silently ignored: an unknown keyword in the schema is a
hard error, so a typo in the schema cannot weaken validation. Unknown keywords
are reported and the document is treated as invalid (fail closed).

CLI:
  aa_perf_schema.py validate <schema.json> <document.json>
  aa_perf_schema.py validate-default <document.json>   # bundled schema next to this file

Exit: 0 valid, 1 invalid (errors on stderr), 2 usage or I/O error.
"""
from __future__ import annotations

import json
import math
import re
import sys
from datetime import datetime
from pathlib import Path

_HERE = Path(__file__).resolve().parent
DEFAULT_SCHEMA = _HERE / "aa-projection-perf.schema.json"

# Keywords this validator understands. Any other keyword is a hard error.
_KNOWN_KEYWORDS = frozenset(
    {
        "$schema",
        "$id",
        "$defs",
        "$ref",
        "title",
        "description",
        "type",
        "required",
        "properties",
        "additionalProperties",
        "enum",
        "const",
        "minimum",
        "maximum",
        "exclusiveMinimum",
        "exclusiveMaximum",
        "minItems",
        "maxItems",
        "uniqueItems",
        "items",
        "minLength",
        "maxLength",
        "pattern",
        "format",
        "allOf",
        "anyOf",
        "oneOf",
        "not",
    }
)

_TYPES = frozenset({"object", "array", "string", "number", "integer", "boolean", "null"})
_MAX_DEPTH = 64


class SchemaError(ValueError):
    """The schema document itself is malformed or unsupported."""


def _json_type(value: object) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    if isinstance(value, dict):
        return "object"
    raise SchemaError(f"value has no JSON type: {type(value).__name__}")


def _type_matches(value: object, wanted: str) -> bool:
    actual = _json_type(value)
    if wanted == "number":
        return actual in ("number", "integer")
    if wanted == "integer":
        return actual == "integer"
    return actual == wanted


def _resolve_ref(ref: str, root: dict) -> object:
    if not ref.startswith("#/"):
        raise SchemaError(f"only local $ref pointers are supported: {ref!r}")
    node: object = root
    for raw in ref[2:].split("/"):
        token = raw.replace("~1", "/").replace("~0", "~")
        if not isinstance(node, dict) or token not in node:
            raise SchemaError(f"unresolvable $ref: {ref!r}")
        node = node[token]
    return node


def _check_keywords(schema: dict, path: str) -> None:
    unknown = sorted(set(schema) - _KNOWN_KEYWORDS)
    if unknown:
        raise SchemaError(f"{path}: unsupported schema keyword(s): {', '.join(unknown)}")


def _check_format(instance: str, fmt: str) -> list[str]:
    if fmt != "date-time":
        return []  # other formats are annotations; nothing to assert
    candidate = instance[:-1] + "+00:00" if instance.endswith("Z") else instance
    try:
        datetime.fromisoformat(candidate)
    except ValueError:
        return [f"not a date-time: {instance!r}"]
    return []


def _validate(instance: object, schema: object, root: dict, path: str, depth: int) -> list[str]:
    if depth > _MAX_DEPTH:
        raise SchemaError(f"{path}: schema recursion exceeded {_MAX_DEPTH}")
    if schema is True:
        return []
    if schema is False:
        return [f"{path}: schema is false"]
    if not isinstance(schema, dict):
        raise SchemaError(f"{path}: schema must be an object, got {_json_type(schema)}")
    _check_keywords(schema, path)

    errors: list[str] = []

    if "$ref" in schema:
        errors.extend(_validate(instance, _resolve_ref(schema["$ref"], root), root, path, depth + 1))

    if "type" in schema:
        wanted = schema["type"]
        wanted_list = [wanted] if isinstance(wanted, str) else wanted
        if not isinstance(wanted_list, list) or not all(
            isinstance(w, str) and w in _TYPES for w in wanted_list
        ):
            raise SchemaError(f"{path}: invalid 'type': {wanted!r}")
        if not any(_type_matches(instance, w) for w in wanted_list):
            errors.append(f"{path}: expected type {wanted_list}, got {_json_type(instance)}")
            return errors  # type mismatch makes sibling assertions meaningless

    if "enum" in schema and instance not in schema["enum"]:
        errors.append(f"{path}: value {instance!r} not in enum {schema['enum']!r}")
    if "const" in schema and instance != schema["const"]:
        errors.append(f"{path}: value {instance!r} != const {schema['const']!r}")

    for keyword, is_exclusive, lower in (
        ("minimum", False, True),
        ("exclusiveMinimum", True, True),
        ("maximum", False, False),
        ("exclusiveMaximum", True, False),
    ):
        if keyword not in schema:
            continue
        bound = schema[keyword]
        if not isinstance(bound, (int, float)) or isinstance(bound, bool):
            raise SchemaError(f"{path}: {keyword} must be a number")
        if not isinstance(instance, (int, float)) or isinstance(instance, bool):
            continue
        if lower and (instance < bound or (is_exclusive and instance == bound)):
            errors.append(f"{path}: {instance} violates {keyword} {bound}")
        if not lower and (instance > bound or (is_exclusive and instance == bound)):
            errors.append(f"{path}: {instance} violates {keyword} {bound}")

    if "minLength" in schema and isinstance(instance, str) and len(instance) < schema["minLength"]:
        errors.append(f"{path}: string shorter than minLength {schema['minLength']}")
    if "maxLength" in schema and isinstance(instance, str) and len(instance) > schema["maxLength"]:
        errors.append(f"{path}: string longer than maxLength {schema['maxLength']}")
    if "pattern" in schema and isinstance(instance, str):
        if re.search(schema["pattern"], instance) is None:
            errors.append(f"{path}: string does not match pattern {schema['pattern']!r}")
    if "format" in schema and isinstance(instance, str):
        errors.extend(f"{path}: {e}" for e in _check_format(instance, schema["format"]))

    if isinstance(instance, list):
        if "minItems" in schema and len(instance) < schema["minItems"]:
            errors.append(f"{path}: fewer than minItems {schema['minItems']}")
        if "maxItems" in schema and len(instance) > schema["maxItems"]:
            errors.append(f"{path}: more than maxItems {schema['maxItems']}")
        if schema.get("uniqueItems") and len({json.dumps(x, sort_keys=True) for x in instance}) != len(instance):
            errors.append(f"{path}: items are not unique")
        if "items" in schema:
            for index, item in enumerate(instance):
                errors.extend(_validate(item, schema["items"], root, f"{path}[{index}]", depth + 1))

    if isinstance(instance, dict):
        for key in schema.get("required", []):
            if key not in instance:
                errors.append(f"{path}: missing required property {key!r}")
        properties = schema.get("properties", {})
        for key, subschema in properties.items():
            if key in instance:
                errors.extend(_validate(instance[key], subschema, root, f"{path}.{key}", depth + 1))
        additional = schema.get("additionalProperties", True)
        if additional is not True:
            for key in instance:
                if key in properties:
                    continue
                if additional is False:
                    errors.append(f"{path}: unexpected property {key!r}")
                else:
                    errors.extend(
                        _validate(instance[key], additional, root, f"{path}.{key}", depth + 1)
                    )

    for keyword, mode in (("allOf", "all"), ("anyOf", "any"), ("oneOf", "one")):
        if keyword not in schema:
            continue
        branches = schema[keyword]
        if not isinstance(branches, list):
            raise SchemaError(f"{path}: {keyword} must be an array")
        branch_errors = [
            _validate(instance, branch, root, path, depth + 1) for branch in branches
        ]
        passing = sum(1 for e in branch_errors if not e)
        if mode == "all":
            for e in branch_errors:
                errors.extend(e)
        elif mode == "any" and passing == 0:
            errors.append(f"{path}: no anyOf branch matched ({len(branches)} tried)")
        elif mode == "one" and passing != 1:
            errors.append(f"{path}: expected exactly one oneOf branch, {passing} matched")

    if "not" in schema and not _validate(instance, schema["not"], root, path, depth + 1):
        errors.append(f"{path}: value matches a 'not' schema")

    return errors


def validate(instance: object, schema: dict, root: dict | None = None) -> list[str]:
    """Return a list of human-readable errors; empty means valid."""
    if not isinstance(schema, dict):
        raise SchemaError("schema must be a JSON object")
    _, numeric_errors = finite_numbers(instance)
    return numeric_errors + _validate(instance, schema, root if root is not None else schema, "$", 0)


def finite_numbers(instance, path: str = "$") -> tuple:
    """Copy JSON data, replacing non-finite numbers with null and recording paths."""
    errors: list[str] = []
    if isinstance(instance, (int, float)) and not isinstance(instance, bool):
        try:
            finite = math.isfinite(instance)
        except OverflowError:
            finite = False
        if not finite:
            return None, [f"{path}: non-finite number"]
    if isinstance(instance, dict):
        clean = {}
        for key, value in instance.items():
            clean[key], child_errors = finite_numbers(value, f"{path}.{key}")
            errors.extend(child_errors)
        return clean, errors
    if isinstance(instance, list):
        clean_list = []
        for index, value in enumerate(instance):
            child, child_errors = finite_numbers(value, f"{path}[{index}]")
            clean_list.append(child)
            errors.extend(child_errors)
        return clean_list, errors
    return instance, errors


def load_schema(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise SchemaError(f"cannot read schema {path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise SchemaError(f"schema {path} is not valid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise SchemaError(f"schema {path} must be a JSON object")
    return data


def _cli(argv: list[str]) -> int:
    if not argv:
        print(__doc__.splitlines()[0], file=sys.stderr)
        print("usage: aa_perf_schema.py validate <schema> <document>", file=sys.stderr)
        print("       aa_perf_schema.py validate-default <document>", file=sys.stderr)
        return 2
    command = argv[0]
    if command == "validate":
        if len(argv) != 3:
            print("usage: aa_perf_schema.py validate <schema> <document>", file=sys.stderr)
            return 2
        schema_path, document_path = Path(argv[1]), Path(argv[2])
    elif command == "validate-default":
        if len(argv) != 2:
            print("usage: aa_perf_schema.py validate-default <document>", file=sys.stderr)
            return 2
        schema_path, document_path = DEFAULT_SCHEMA, Path(argv[1])
    else:
        print(f"unknown command: {command}", file=sys.stderr)
        return 2

    try:
        schema = load_schema(schema_path)
        document = json.loads(document_path.read_text(encoding="utf-8"))
    except (SchemaError, json.JSONDecodeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2

    try:
        errors = validate(document, schema)
    except SchemaError as exc:
        print(f"schema error: {exc}", file=sys.stderr)
        return 2

    if errors:
        print(f"invalid: {document_path}", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1
    print(f"valid: {document_path}")
    return 0


if __name__ == "__main__":
    sys.exit(_cli(sys.argv[1:]))
