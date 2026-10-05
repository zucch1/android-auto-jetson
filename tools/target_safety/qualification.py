"""Local-only source audit; shared receipt shape for stage1 qualification capture."""
from __future__ import annotations

import ast
import hashlib
from pathlib import Path
from typing import TypedDict


class Receipt(TypedDict):
    argv: list[str]
    command: str
    cwd: str
    exit_status: int
    stdout: str
    stderr: str


class SourceCheck(TypedDict):
    path: str
    sha256: str
    pure_loc: int


class ModuleLimitError(RuntimeError):
    def __init__(self, path: Path) -> None:
        self.path = path
        super().__init__(str(path))


def source_checks() -> list[SourceCheck]:
    """AST-parse, hash and enforce the existing <250 pure-LOC source gate."""
    here = Path(__file__).resolve().parent
    sources: list[SourceCheck] = []
    for path in sorted((*here.glob("*.py"), *(here.parents[1] / "tests/target_safety").glob("*.py"))):
        source = path.read_text()
        ast.parse(source)
        pure_loc = sum(bool(line.strip()) and not line.lstrip().startswith("#")
                       for line in source.splitlines())
        if pure_loc >= 250:
            raise ModuleLimitError(path)
        sources.append(SourceCheck(path=str(path), sha256=hashlib.sha256(source.encode()).hexdigest(),
                                   pure_loc=pure_loc))
    return sources
