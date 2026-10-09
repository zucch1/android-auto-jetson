# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Invoked by contamination.py with retained compiler dependencies and linker map.
"""Classify and hash actual selected inputs; architecture alone is not provenance."""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
from typing import assert_never

from tools.build.validate_sysroot import validate_payload
from tools.build.decode_overlay import Overlay, verify
from tools.sysroot.models import Code, Provenance, SysrootError


@dataclass(frozen=True, slots=True)
class Inputs:
    sysroot: Path
    build: Path
    source: Path
    compiler: Path
    manifest: Path | None
    payload: Path | None
    overlay: Overlay | None = None


@dataclass(frozen=True, slots=True)
class SelectedInput:
    path: str
    resolved: str
    sha256: str
    category: str


@dataclass(frozen=True, slots=True)
class InputReport:
    acceptance: str
    selected: tuple[SelectedInput, ...]
    contamination: tuple[str, ...]
    overlay: Overlay | None = None


def compiler_path(compiler: Path, option: str) -> Path:
    result = subprocess.run([str(compiler), option], capture_output=True, text=True,
                            check=True, timeout=30)
    return Path(result.stdout.strip()).resolve(strict=True)


def check_inputs(binary: Path, inputs: Inputs) -> InputReport:
    """Inspect retained system-header dependencies and actual linker LOAD inputs."""
    sysroot = inputs.sysroot.resolve(strict=True)
    build = inputs.build.resolve(strict=True)
    source = inputs.source.resolve(strict=True)
    overlay_paths: frozenset[Path] = frozenset()
    record = subprocess.run(['readelf', '-p', '.aa_decode_overlay', str(binary)],
                            capture_output=True, text=True, check=True, timeout=30)
    digests = re.findall(r'\[\s*[0-9a-f]+\]\s+([0-9a-f]{64})\s*$', record.stdout, re.MULTILINE)
    if "String dump of section '.aa_decode_overlay':" in record.stdout and inputs.overlay is None:
        raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.overlay-arguments')
    if inputs.overlay is not None:
        overlay_paths = verify(inputs.overlay)
        if digests != [inputs.overlay.digest]:
            raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.overlay-binary-binding')
    builtin = compiler_path(inputs.compiler, '-print-file-name=include')
    support = compiler_path(inputs.compiler, '-print-libgcc-file-name').parent
    allowed_support = {support / name for name in
                       ('libgcc.a', 'crtbegin.o', 'crtend.o', 'crtbeginS.o', 'crtendS.o')}
    executables = {inputs.compiler.resolve(strict=True)} | {
        compiler_path(inputs.compiler, f'-print-prog-name={name}')
        for name in ('cc1plus', 'collect2', 'as', 'ld')}
    validated: set[Path] = set()
    observed = False
    acceptance = 'fixture-link-sysroot-not-target'
    if inputs.manifest is not None and inputs.payload is not None:
        manifest, _ = validate_payload(inputs.manifest, inputs.payload)
        match manifest.provenance:
            case Provenance.OBSERVED:
                if inputs.payload.resolve(strict=True) != sysroot:
                    raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.sysroot-binding')
                acceptance = 'target-observed-selected-inputs'
                observed = True
                validated = {sysroot / str(entry.staged_path) for entry in manifest.files}
            case Provenance.FIXTURE:
                acceptance = 'fixture-link-sysroot-not-target'
            case unreachable:
                assert_never(unreachable)
    link_map = binary.with_name(binary.name + '.map')
    if not link_map.is_file():
        raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.missing-dependencies-or-link-map')
    map_text = link_map.read_text()
    loads = tuple(Path(token) if Path(token).is_absolute() else build / token
                  for token in re.findall(r'^LOAD (.+)$', map_text, re.MULTILINE)
                  if token != 'linker stubs')
    # Select only this link's project objects, including objects packed into its
    # project archives. Match archive bytes, not just basenames shared by targets.
    objects = {path for path in loads if path.suffix == '.o'
               and (path.is_relative_to(build) or path.is_relative_to(source))}
    archives = tuple(path for path in loads if path.suffix == '.a'
                     and (path.is_relative_to(build) or path.is_relative_to(source)))
    if archives:
        commands = json.loads((build / 'compile_commands.json').read_text())
        candidates: set[Path] = set()
        for command in commands:
            flags = shlex.split(command['command'])
            output = Path(flags[flags.index('-o') + 1])
            candidates.add(output if output.is_absolute() else Path(command['directory']) / output)
        for archive in archives:
            members = subprocess.check_output(['ar', 't', str(archive)], text=True, timeout=30).splitlines()
            for member in members:
                packed = subprocess.check_output(['ar', 'p', str(archive), member], timeout=30)
                matches = {path for path in candidates if path.name == member and path.is_file()
                           and path.read_bytes() == packed}
                if len(matches) != 1:
                    raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.archive-object-binding')
                objects.update(matches)
    dependencies = tuple(path.with_name(path.name + '.headers.d') for path in sorted(objects))
    if not dependencies or any(not path.is_file() for path in dependencies):
        raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.missing-dependencies-or-link-map')
    paths: set[Path] = set(executables)
    for dependency in dependencies:
        text = dependency.read_text().replace('\\\n', '')
        for token in shlex.split(text.split(':', 1)[1]):
            path = Path(token)
            paths.add(path if path.is_absolute() else build / path)
    # LOAD records include scripts, archives and their resolved shared-object members.
    paths.update(loads)
    selected: list[SelectedInput] = []
    contamination: list[str] = []
    target_headers = False
    target_libraries = False
    for path in sorted(paths):
        path = Path(os.path.abspath(path))
        resolved = path.resolve(strict=True)
        category = 'unvalidated-external'
        if resolved in executables:
            category = 'compiler-executable'
        elif resolved.is_relative_to(builtin) or resolved in allowed_support:
            category = 'compiler-intrinsic-support'
        elif resolved.is_relative_to(sysroot):
            category = 'target-sysroot'
            target_headers |= resolved.is_relative_to(sysroot / 'usr/include')
            target_libraries |= resolved.suffix in ('.o', '.a') or '.so' in resolved.name
            if observed and (path not in validated or resolved not in validated):
                contamination.append(f'unrecorded-target-input:{path}')
        elif inputs.overlay is not None and resolved.is_relative_to(inputs.overlay.root):
            category = 'digest-recorded-decode-overlay'
            if path not in overlay_paths or resolved not in overlay_paths:
                contamination.append(f'unrecorded-overlay-input:{path}')
        elif resolved.is_relative_to(source) or resolved.is_relative_to(build):
            category = 'project-or-staged-dependency'
        else:
            contamination.append(f'external-input:{path}->{resolved}')
        with resolved.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        selected.append(SelectedInput(str(path), str(resolved), digest, category))
    if not target_headers or not target_libraries:
        contamination.append('missing-target-header-or-library-evidence')
    if inputs.overlay is not None:
        acceptance += '-with-digest-recorded-overlay-not-target-qualified'
    return InputReport(acceptance, tuple(selected), tuple(contamination), inputs.overlay)
