# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# Import tools.sysroot.stage; CLI: python3 -B tools/sysroot/cli.py --help
"""Local byte copying and versioned immutable publication; no acquisition or qualification."""
from __future__ import annotations

from contextlib import ExitStack
from dataclasses import dataclass
import fcntl
import os
from pathlib import Path
import shutil
import stat
import tempfile
from typing import Literal, assert_never

from .filesystem import DIR_FLAGS, directory, regular, validate_tree
from .manifest import encode_manifest, manifest_digest, parse_manifest
from .models import Code, Digest, Manifest, RegularFile, Symlink, SysrootError
from .publication import publish


@dataclass(frozen=True, slots=True)
class StagedArtifact:
    artifact: Path
    manifest: Manifest
    manifest_sha256: Digest
    reused: bool
    capability: Literal['pending'] = 'pending'

    @property
    def payload_root(self) -> Path:
        return self.artifact / 'rootfs'


def load_metadata(path: Path) -> Manifest:
    try:
        with ExitStack() as stack:
            descriptor = directory(path.absolute().parent, stack)
            return parse_manifest(regular(descriptor, path.name, stack).read())
    except OSError as error:
        raise SysrootError(Code.LOCAL_IO, str(path)) from error


def validate_artifact(artifact: Path) -> StagedArtifact:
    """Check local integrity only; consumers must separately bind the returned digest."""
    try:
        with ExitStack() as stack:
            descriptor = directory(artifact, stack)
            if set(os.listdir(descriptor)) != {'manifest.json', 'rootfs'} or stat.S_IMODE(
                    os.fstat(descriptor).st_mode) != 0o555:
                raise SysrootError(Code.ARTIFACT_MISMATCH, 'artifact')
            stream = regular(descriptor, 'manifest.json', stack)
            if stat.S_IMODE(os.fstat(stream.fileno()).st_mode) != 0o444:
                raise SysrootError(Code.ARTIFACT_MISMATCH, 'manifest.mode')
            raw = stream.read()
            manifest = parse_manifest(raw)
            if raw != encode_manifest(manifest) or artifact.name != manifest.version:
                raise SysrootError(Code.ARTIFACT_MISMATCH, 'manifest.binding')
            payload = os.open('rootfs', DIR_FLAGS, dir_fd=descriptor)
            stack.callback(os.close, payload)
            validate_tree(payload, manifest, staged=True)
            return StagedArtifact(artifact.absolute(), manifest, manifest_digest(manifest), True)
    except (SysrootError, OSError) as error:
        raise SysrootError(Code.ARTIFACT_MISMATCH, str(artifact)) from error


def copy_payload(source: int, payload: Path, manifest: Manifest) -> None:
    for entry in manifest.files:
        output = payload / entry.staged_path
        output.parent.mkdir(parents=True, exist_ok=True)
        match entry:
            case RegularFile():
                with ExitStack() as stack:
                    stream = regular(source, entry.original_input_path[1:], stack)
                    with output.open('xb') as target:
                        shutil.copyfileobj(stream, target)
            case Symlink(link_text=link):
                output.symlink_to(link)
            case unreachable:
                assert_never(unreachable)


def freeze(artifact: Path) -> None:
    for root, dirs, files in os.walk(artifact, followlinks=False):
        for name in files:
            path = Path(root) / name
            if not path.is_symlink():
                path.chmod(0o444)
        for name in dirs:
            (Path(root) / name).chmod(0o555)
    artifact.chmod(0o555)


def _materialize(snapshot: Path, metadata: Path, destination: Path) -> StagedArtifact:
    """Copy an explicit local snapshot; destination must be an existing trusted parent.

    Directory flock serializes cooperating local publishers, including reuse. Modes
    prevent accidental writes, not writes by an owner who deliberately changes modes.
    """
    manifest = load_metadata(metadata)
    with ExitStack() as stack:
        source = directory(snapshot, stack)
        parent = directory(destination, stack)
        if destination.absolute().is_relative_to(snapshot.absolute()):
            raise SysrootError(Code.UNSAFE_PATH, 'destination inside snapshot')
        fcntl.flock(parent, fcntl.LOCK_EX)
        validate_tree(source, manifest)
        output = destination.absolute() / manifest.version
        if os.path.lexists(output):
            existing = validate_artifact(output)
            if existing.manifest != manifest:
                raise SysrootError(Code.NEW_VERSION_REQUIRED, manifest.version)
            validate_tree(source, manifest)
            return existing
        # /proc/self/fd anchors scratch creation to the already opened destination.
        with tempfile.TemporaryDirectory(prefix='.sysroot-', dir=f'/proc/self/fd/{parent}') as temp:
            scratch = destination.absolute() / Path(temp).name
            artifact = scratch
            payload = artifact / 'rootfs'
            payload.mkdir()
            copy_payload(source, payload, manifest)
            (artifact / 'manifest.json').write_bytes(encode_manifest(manifest))
            validate_tree(source, manifest)
            freeze(artifact)
            payload_fd = directory(payload, stack)
            validate_tree(payload_fd, manifest, staged=True)
            if os.path.lexists(output):
                raise SysrootError(Code.NEW_VERSION_REQUIRED, manifest.version)
            publish(parent, scratch.name, manifest.version)
        return StagedArtifact(output, manifest, manifest_digest(manifest), False)


def materialize(snapshot: Path, metadata: Path, destination: Path) -> StagedArtifact:
    """Return a validated artifact or a typed SysrootError; never repair existing versions."""
    try:
        return _materialize(snapshot, metadata, destination)
    except OSError as error:
        raise SysrootError(Code.LOCAL_IO, str(error.filename)) from error
