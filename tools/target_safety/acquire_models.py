"""Value types for the task-5 target acquisition route and its observation parse.

Pure immutable record types (acquisition record, copied entries, observed
identity, quiet boundary, window request/result) plus the one-shot boundary parse
of the captured observation (selected inputs + cached dpkg ownership + identity).
Route behavior lives in the sibling acquire module; selection policy in
acquire_input_select; manifest assembly in acquire_manifest. Stdlib-only and
bundle-safe.
"""
from __future__ import annotations

import json
import time
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Final

from .kernel import Blocked
from .snapshot_diff import DiffResult
from .snapshot_models import LinkException

RECORD_SCHEMA: Final = "aa-acquire/1"
OBSERVATION_SCHEMA: Final = "aa-acquire-observation/1"
DEFAULT_SNAPSHOT_DEADLINE_SECONDS: Final = 900.0
QUIET_NOTE: Final = (
    "Process/cwd observations are proportionate evidence about a quiet "
    "certification window only. status=quiescent means no process with a working "
    "directory under a protected root was observed before or after the window; it "
    "does not prove the absence of concurrent writers. status=unproven-activity "
    "means observed activity prevents asserting a serialized window. The "
    "protected-path before/after equivalence is the safety evidence for no "
    "persistent change within the window; a transient write fully reverted within "
    "the window is an accepted limitation of endpoint-state verification."
)


@dataclass(frozen=True, slots=True)
class InputSpec:
    """One selected needed build input: an absolute target path to acquire."""

    path: str


@dataclass(frozen=True, slots=True)
class PackageInfo:
    """dpkg ownership of one copied input (observed)."""

    name: str
    version: str
    architecture: str
    source: str


@dataclass(frozen=True, slots=True)
class TargetIdentity:
    """dpkg/kernel/JetPack identity observed on the target."""

    kernel: str | None
    l4t: str | None
    jetpack: str | None
    dpkg_summary: tuple[tuple[str, str], ...]


@dataclass(frozen=True, slots=True)
class InputObservation:
    """Selected inputs + cached dpkg ownership + identity (boundary-parsed)."""

    specs: tuple[InputSpec, ...]
    packages: Mapping[str, PackageInfo]
    identity: TargetIdentity
    provenance: str = "observed"


@dataclass(frozen=True, slots=True)
class CopiedEntry:
    """One acquired input: staging + content evidence + dpkg owner."""

    original_input_path: str
    kind: str
    copied_path: str
    sha256: str
    size: int | None
    link_text: str | None
    package: PackageInfo
    original_link_text: str | None = None
    original_link_sha256: str | None = None


@dataclass(frozen=True, slots=True)
class ProcessObservation:
    """A proportionate process/cwd sample (read-only, never writes or kills)."""

    processes: tuple[tuple[str, str, str | None], ...]
    loadavg: tuple[float, float, float] | None
    complete: bool = True


@dataclass(frozen=True, slots=True)
class QuietBoundary:
    """Certification-window quiet evidence; honest about proven vs unproven."""

    before: ProcessObservation
    after: ProcessObservation
    relevant_writers: tuple[str, ...]
    status: str
    proven: bool
    note: str


@dataclass(frozen=True, slots=True)
class AcquisitionRecord:
    """Raw observed acquisition output; assembled into aa-sysroot-1 locally."""

    schema: str
    task_window_id: str
    provenance: str
    identity: TargetIdentity
    entries: tuple[CopiedEntry, ...]
    quiet_boundary: QuietBoundary
    scratch: str
    completed: bool = True
    error: str | None = None
    selected_inputs: tuple[str, ...] = ()
    boundary_samples: tuple[ProcessObservation, ...] = ()

    def jsonable(self) -> dict[str, object]:
        return {
            "schema": self.schema,
            "task_window_id": self.task_window_id,
            "provenance": self.provenance,
            "identity": {
                "kernel": self.identity.kernel,
                "l4t": self.identity.l4t,
                "jetpack": self.identity.jetpack,
                "dpkg_summary": [list(pair) for pair in self.identity.dpkg_summary],
            },
            "entries": [
                {
                    "original_input_path": entry.original_input_path,
                    "kind": entry.kind,
                    "copied_path": entry.copied_path,
                    "sha256": entry.sha256,
                    "size": entry.size,
                    "link_text": entry.link_text,
                    "original_link_text": entry.original_link_text,
                    "original_link_sha256": entry.original_link_sha256,
                    "package": {
                        "name": entry.package.name,
                        "version": entry.package.version,
                        "architecture": entry.package.architecture,
                        "source": entry.package.source,
                    },
                }
                for entry in self.entries
            ],
            "quiet_boundary": _quiet_jsonable(self.quiet_boundary),
            "scratch": self.scratch,
            "completed": self.completed,
            "error": self.error,
            "selected_inputs": list(self.selected_inputs),
            "boundary_samples": [
                {"processes": [list(p) for p in sample.processes], "complete": sample.complete}
                for sample in self.boundary_samples],
        }


def _quiet_jsonable(quiet: QuietBoundary) -> dict[str, object]:
    def obs(view: ProcessObservation) -> dict[str, object]:
        return {
            "processes": [list(p) for p in view.processes],
            "loadavg": list(view.loadavg) if view.loadavg is not None else None,
            "complete": view.complete,
        }
    return {
        "before": obs(quiet.before),
        "after": obs(quiet.after),
        "relevant_writers": list(quiet.relevant_writers),
        "status": quiet.status,
        "proven": quiet.proven,
        "note": quiet.note,
    }


@dataclass(frozen=True, slots=True)
class WindowRequest:
    """Typed target-window inputs. A genuine domain bundle, not a throwaway."""

    roots: tuple[Path, ...]
    link_exceptions: tuple[LinkException, ...]
    input_set: tuple[InputSpec, ...]
    scratch: Path
    task_window_id: str
    target_identity: str
    package_of: Callable[[str], PackageInfo]
    identity: Callable[[], TargetIdentity]
    now_ns: Callable[[], int] = time.time_ns
    snapshot_deadline_seconds: float = DEFAULT_SNAPSHOT_DEADLINE_SECONDS
    select: Callable[[], InputObservation] | None = None
    provenance: str = "observed"


@dataclass(frozen=True, slots=True)
class WindowResult:
    """Where the window's evidence landed and its equivalence verdict."""

    scratch: Path
    snapshot_dir: Path
    before_manifest: Path
    after_manifest: Path
    diff_artifact: Path
    receipt: Path
    record_path: Path
    diff: DiffResult
    record: AcquisitionRecord
    acquire_error: Blocked | None = None


def _package(mapping: Mapping[str, object], path: str) -> PackageInfo:
    record = mapping.get(path)
    if not isinstance(record, dict):
        raise Blocked("observation_package_missing", path)
    name, version, arch, source = (record.get(k) for k in ("name", "version", "architecture", "source"))
    if not all(isinstance(v, str) and v for v in (name, version, arch, source)):
        raise Blocked("observation_package_malformed", path)
    return PackageInfo(name, version, arch, source)


def _identity(mapping: Mapping[str, object]) -> TargetIdentity:
    record = mapping.get("identity")
    if not isinstance(record, dict):
        raise Blocked("observation_identity_malformed", "identity")
    def opt(key: str) -> str | None:
        value = record.get(key)
        return value if isinstance(value, str) else None
    raw_summary = record.get("dpkg_summary", [])
    summary: list[tuple[str, str]] = []
    if isinstance(raw_summary, list):
        for pair in raw_summary:
            if isinstance(pair, list) and len(pair) == 2 and all(isinstance(v, str) for v in pair):
                summary.append((pair[0], pair[1]))
    return TargetIdentity(opt("kernel"), opt("l4t"), opt("jetpack"), tuple(summary))


def load_input_set(path: Path) -> InputObservation:
    """Parse the captured observation once at the boundary; cached, exact, no glob."""
    raw = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(raw, dict) or raw.get("schema") != OBSERVATION_SCHEMA:
        raise Blocked("observation_malformed", str(path))
    inputs = raw.get("inputs")
    if not isinstance(inputs, list) or not inputs:
        raise Blocked("observation_malformed", "inputs")
    specs: list[InputSpec] = []
    seen: set[str] = set()
    for item in inputs:
        if not isinstance(item, dict) or not isinstance(item.get("path"), str):
            raise Blocked("observation_malformed", "input.path")
        target = item["path"]
        if not target.startswith("/") or target in seen:
            raise Blocked("observation_unsafe", target)
        seen.add(target)
        specs.append(InputSpec(target))
    packages_raw = raw.get("packages")
    if not isinstance(packages_raw, dict):
        raise Blocked("observation_malformed", "packages")
    packages = {p: _package(packages_raw, p) for p in (s.path for s in specs)}
    return InputObservation(tuple(specs), packages, _identity(raw))
