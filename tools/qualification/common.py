# SPDX-License-Identifier: GPL-3.0-or-later
"""Small stdlib-only JSON boundary, matching the existing evidence tools."""
from __future__ import annotations

import json
import math
import re
import stat
from dataclasses import dataclass
from pathlib import Path
from typing import Final, TypeAlias

JSON: TypeAlias = 'None | bool | int | float | str | list[JSON] | dict[str, JSON]'
DIGEST: Final = re.compile(r'(?:sha256:)?[0-9a-f]{64}\Z')


@dataclass(frozen=True, slots=True)
class EvidenceError(Exception):
    code: str

    def __str__(self) -> str:
        return self.code


def require(condition: bool, code: str) -> None:
    if not condition:
        raise EvidenceError(code)


def unique_pairs(pairs: list[tuple[str, JSON]]) -> dict[str, JSON]:
    result: dict[str, JSON] = {}
    for key, value in pairs:
        require(key not in result, 'DUPLICATE_JSON_KEY')
        result[key] = value
    return result


def invalid_constant(_value: str) -> None:
    raise EvidenceError('NONFINITE_JSON')


@dataclass(frozen=True, slots=True)
class Document:
    """Untrusted JSON is accessed only through strict, non-coercing readers."""
    values: dict[str, JSON]

    @classmethod
    def parse(cls, value: JSON) -> Document:
        if isinstance(value, dict):
            return cls(value)
        raise EvidenceError('EXPECTED_OBJECT')

    def get(self, key: str) -> JSON:
        require(key in self.values, 'MISSING_FIELD:' + key)
        return self.values[key]

    def text(self, key: str) -> str:
        value = self.get(key)
        if isinstance(value, str) and value.strip():
            require(value.strip().lower() not in {'unknown', 'pending', 'tbd', 'n/a'},
                    'PLACEHOLDER:' + key)
            return value
        raise EvidenceError('EXPECTED_TEXT:' + key)

    def number(self, key: str) -> float:
        value = self.get(key)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            try:
                require(math.isfinite(value), 'NONFINITE_NUMBER:' + key)
                return float(value)
            except OverflowError as error:
                raise EvidenceError('NONFINITE_NUMBER:' + key) from error
        raise EvidenceError('EXPECTED_NUMBER:' + key)

    def integer(self, key: str) -> int:
        value = self.get(key)
        if type(value) is int:
            return value
        raise EvidenceError('EXPECTED_INTEGER:' + key)

    def equal(self, key: str, expected: JSON) -> None:
        value = self.get(key)
        require(type(value) is type(expected) and value == expected, 'INVALID_FIELD:' + key)

    def child(self, key: str) -> Document:
        return Document.parse(self.get(key))

    def array(self, key: str) -> list[JSON]:
        value = self.get(key)
        if isinstance(value, list):
            return value
        raise EvidenceError('EXPECTED_ARRAY:' + key)

    def keys(self, expected: set[str]) -> None:
        require(set(self.values) == expected, 'UNEXPECTED_OR_MISSING_FIELDS')


def load(path: Path) -> Document:
    return Document.parse(json.loads(path.read_text(encoding='utf-8'),
                                    object_pairs_hook=unique_pairs,
                                    parse_constant=invalid_constant))


def package_file(root: Path, relative: str) -> Path:
    """Reject traversal, links (including ancestors), and special files before I/O."""
    path = Path(relative)
    require(not path.is_absolute() and bool(path.parts) and '..' not in path.parts,
            'UNSAFE_ARTIFACT_PATH')
    target = root
    for part in path.parts:
        target = target / part
        require(not target.is_symlink(), 'SYMLINK_REJECTED')
    require(target.resolve().is_relative_to(root.resolve()), 'ARTIFACT_OUTSIDE_PACKAGE')
    require(stat.S_ISREG(target.stat().st_mode), 'NON_REGULAR_FILE')
    return target
