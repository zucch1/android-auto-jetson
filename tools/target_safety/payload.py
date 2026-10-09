"""Deterministic stdlib-only in-memory loader and remote payload assembly.

Payload bytes are built from local source text and executed from stdin memory
only; no remote product file is ever read. Module tuples carry dependencies in
load order. The sizing guard payload omits inventory and probes entirely, so
sizing is structurally isolated from content, Git and package observations.

Assembly order is diagnostic-critical: the bounded-write discipline module
(egress) loads first so the minimal entry record is the earliest executable
bounded write in the bundled payload - before every other target_safety module
source is executed - followed by the metadata record and the module_load stage
bracket pair. CPython compiles the complete stdin before the first statement
runs, so the entry record also marks interpreter/source ingestion done; its
absence never proves execution absence, only that no entry record was received.
"""
from __future__ import annotations

import ast
from pathlib import Path
from typing import Final

if __package__: from .report import SIZING_MODE
else: from report import SIZING_MODE

HERE: Final = Path(__file__).resolve().parent
# Dependency order: leaf modules first; every remote payload carries the
# resource/bootstrap/sizing/collector modules, with collector_io ahead of its
# collector consumer and the pure sizing models ahead of coverage. The sizing
# guard payload omits inventory and probes entirely (structural isolation); the
# supervisor carries the new typed sizing evidence layer strictly before its
# report consumer. egress (the bounded-write discipline) is listed with its
# consumers and hoisted first by bundle() below.
MODULES: Final = ("resource", "bootstrap", "kernel", "collector_io", "collector", "sizing_models",
                  "coverage", "inventory", "probes", "sizing")
SIZING_GUARD_MODULES: Final = ("resource", "bootstrap", "kernel", "collector_io", "collector",
                               "sizing_models", "coverage", "egress", "sizing")
SUPERVISOR_MODULES: Final = ("resource", "bootstrap", "kernel", "deadline", "collector_io",
                             "collector", "sizing_models", "coverage", "inventory", "probes",
                             "sizing_records", "sizing_validation", "guard", "report",
                             "setting_process", "policy", "setting", "egress", "restoration",
                             "sizing", "supervisor")


def bundle(modules: tuple[str, ...] = MODULES,
           tail: str = "raise SystemExit(target_safety.probes.main())") -> bytes:
    """Load stdlib-only modules from stdin memory, never remote product files."""
    ordered = ("egress",) + tuple(name for name in modules if name != "egress")
    lines = ["import sys, types", "p = types.ModuleType('target_safety')",
             "p.__path__ = []", "sys.modules[p.__name__] = p", "import target_safety"]
    for name in ordered:
        source = (HERE / f"{name}.py").read_text()
        ast.parse(source)
        lines.extend([f"m = types.ModuleType('target_safety.{name}')",
                      f"m.__package__ = 'target_safety'", "sys.modules[m.__name__] = m",
                      f"setattr(p, {name!r}, m)",
                      f"exec(compile({source!r}, '<stdin>/{name}.py', 'exec'), m.__dict__)"])
        if name == "egress":
            lines.extend(["target_safety.egress.writer().entry()",
                          "target_safety.egress.writer().meta()",
                          "target_safety.egress.writer().stage_started('module_load')"])
    lines.extend(["target_safety.egress.writer().stage_completed('module_load')", tail])
    return ("\n".join(lines) + "\n").encode()


def _supervisor_payload(guard: bytes, entry: str) -> bytes:
    tail = ("import target_safety.supervisor as _S\n"
            f"_S.GUARD_BUNDLE = {guard!r}\n"
            f"raise SystemExit({entry})")
    return bundle(SUPERVISOR_MODULES, tail)


def supervisor_bundle() -> bytes:
    """Supervisor bundle carrying the original in-memory guard bundle (no disk)."""
    return _supervisor_payload(bundle(), "_S.main()")


def sizing_guard_bundle() -> bytes:
    """Distinct sizing guard bundle: topology only, no inventory/probes modules."""
    return bundle(SIZING_GUARD_MODULES, "raise SystemExit(target_safety.sizing.main())")


def sizing_bundle() -> bytes:
    """Distinct sizing supervisor bundle bound to the topology-sizing mode."""
    return _supervisor_payload(sizing_guard_bundle(), f"_S.main(mode={SIZING_MODE!r})")
