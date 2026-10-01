# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/deps/check_manifest.py --lock deps/manifest.lock [--archive ARCHIVE]
"""Check dependency identities and raw-byte lock integrity without network access."""
from __future__ import annotations

import argparse
from collections.abc import Mapping
from dataclasses import dataclass
from enum import StrEnum
import hashlib
import json
from pathlib import Path
import re
from typing import Final, assert_never

type JsonValue = str | int | float | bool | None | list[JsonValue] | dict[str, JsonValue]


@dataclass(frozen=True, slots=True)
class ManifestError(Exception):
    code: str
    field: str

    def __str__(self) -> str:
        return f'{self.code}\t{self.field}'


@dataclass(frozen=True, slots=True)
class Source:
    name: str
    commit: str
    archive_sha256: str
    url: str


SOURCES: Final = (
    Source('aasdk', '9bf6adf933665dee26532201719fac14a047ccf1',
           '78f570189bf504e7323b28cbfa5159b6f16d722a3c547ad3a0f9bdc12a9ca111',
           'https://github.com/opencardev/aasdk.git'),
    Source('oaa', '61eab61c5f9968154ff1a80faa8c0a427b208479',
           'c4ed2e8234c872b3764ba8ca83502e75d9961f388797ba285d2ee65811da8ef2',
           'https://github.com/mrmees/open-android-auto.git'),
    Source('googletest', 'b514bdc898e2951020cbdca1304b75f5950d1f59',
           '7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926',
           'https://github.com/google/googletest.git'),
)
PATCH_SHA256: Final = 'd27b1c7099041042ad255c2cd16b7d61588b6b6aaa93b4c7d1b0c0ffb35b7091'
PATCH_PATH: Final = 'patches/aasdk-googletest.patch'
PACKAGES: Final = frozenset((
    'protobuf-compiler', 'libprotobuf-dev', 'libprotobuf32t64', 'libboost-dev',
    'libboost-system-dev', 'libboost-log-dev', 'libssl-dev', 'libssl3t64',
    'libusb-1.0-0-dev', 'libusb-1.0-0', 'libgstreamer1.0-dev',
    'libgstreamer-plugins-base1.0-dev', 'libgstreamer1.0-0',
    'libgstreamer-plugins-base1.0-0', 'gstreamer1.0-plugins-good',
    'gstreamer1.0-plugins-bad', 'gstreamer1.0-libav', 'gstreamer1.0-qt6',
    'qt6-base-dev', 'qt6-declarative-dev', 'nvidia-l4t-gstreamer',
    'cmake', 'ninja-build', 'pkg-config', 'libabsl-dev',
))
VERSION: Final = re.compile(r'(?:[0-9]+:)?[0-9][0-9A-Za-z.+~]*(?:-[0-9A-Za-z.+~]+)*')


class Availability(StrEnum):
    INSTALLED = 'installed'
    MISSING = 'missing'
    OPTIONAL = 'unavailable-optional'


@dataclass(frozen=True, slots=True)
class Package:
    name: str
    availability: Availability
    installed_version: str | None
    candidate_version: str | None
    architecture: str | None


def mapping(value: JsonValue, field: str) -> Mapping[str, JsonValue]:
    """Narrow untrusted JSON at the parsing boundary."""
    if not isinstance(value, dict):
        raise ManifestError('MALFORMED_VALUE', field)
    return value


def text(value: JsonValue, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ManifestError('MALFORMED_VALUE', field)
    return value


def digest(value: JsonValue, length: int, field: str) -> str:
    result = text(value, field)
    if re.fullmatch(f'[0-9a-f]{{{length}}}', result) is None:
        raise ManifestError('MALFORMED_VALUE', field)
    return result


def version(value: JsonValue, field: str) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str) or VERSION.fullmatch(value) is None:
        raise ManifestError('MALFORMED_VERSION', field)
    return value


def parse_package(value: JsonValue) -> Package:
    record = mapping(value, 'host_packages[]')
    name = text(record.get('name'), 'package.name')
    try:
        status = Availability(text(record.get('status'), f'{name}.status'))
    except ValueError as error:
        raise ManifestError('MALFORMED_VALUE', f'{name}.status') from error
    installed = version(record.get('installed_version'), f'{name}.installed_version')
    candidate = version(record.get('candidate_version'), f'{name}.candidate_version')
    architecture = record.get('architecture')
    if architecture is not None:
        architecture = text(architecture, f'{name}.architecture')
        if re.fullmatch(r'[a-z0-9][a-z0-9-]*', architecture) is None:
            raise ManifestError('MALFORMED_VALUE', f'{name}.architecture')
    package = Package(name, status, installed, candidate, architecture)
    match package.availability:
        case Availability.INSTALLED:
            if installed is None or architecture is None:
                raise ManifestError('MALFORMED_VALUE', f'{name}.installed-binding')
        case Availability.MISSING:
            if installed is not None or architecture is not None:
                raise ManifestError('MALFORMED_VALUE', f'{name}.missing-binding')
        case Availability.OPTIONAL:
            if name != 'nvidia-l4t-gstreamer' or any((installed, candidate, architecture)):
                raise ManifestError('MALFORMED_VALUE', f'{name}.optional-binding')
        case unreachable:
            assert_never(unreachable)
    return package


def load(path: Path) -> Mapping[str, JsonValue]:
    value: JsonValue = json.loads(path.read_bytes())
    return mapping(value, str(path))


def validate_manifest(document: Mapping[str, JsonValue]) -> None:
    """Reject missing inventories and identities independently of the lock."""
    if document.get('schema') != 'aa-dependencies-1':
        raise ManifestError('MALFORMED_VALUE', 'schema')
    sources = mapping(document.get('sources'), 'sources')
    if set(sources) != {source.name for source in SOURCES}:
        raise ManifestError('SOURCE_SET', 'sources')
    for expected in SOURCES:
        source = mapping(sources[expected.name], expected.name)
        commit = digest(source.get('commit'), 40, f'{expected.name}.commit')
        archive = digest(source.get('archive_sha256'), 64, f'{expected.name}.archive_sha256')
        if (commit, archive, source.get('url')) != (
                expected.commit, expected.archive_sha256, expected.url):
            raise ManifestError('SOURCE_IDENTITY', expected.name)
    packages = document.get('host_packages')
    if not isinstance(packages, list):
        raise ManifestError('PACKAGE_SET', 'host_packages')
    parsed = tuple(parse_package(package) for package in packages)
    if len(parsed) != len(PACKAGES) or {package.name for package in parsed} != PACKAGES:
        raise ManifestError('PACKAGE_SET', 'host_packages')
    target = mapping(document.get('target_binding'), 'target_binding')
    for field, expected_value in (
        ('status', 'pending-task5-extraction'), ('architecture', 'arm64'),
        ('sysroot_manifest', 'toolchains/jetson-sysroot-manifest.json'),
        ('sysroot_manifest_sha256', None), ('package_versions', []),
        ('required_components', ['protobuf', 'Boost', 'OpenSSL', 'libusb', 'GStreamer', 'Qt6']),
        ('optional_packages', ['nvidia-l4t-gstreamer']),
    ):
        if field not in target or target[field] != expected_value:
            raise ManifestError('MALFORMED_VALUE', f'target_binding.{field}')
    policy = mapping(document.get('build_policy'), 'build_policy')
    if policy.get('network_fetch') is not False or policy.get('host_full_build') != 'blocked-missing-development-packages':
        raise ManifestError('MALFORMED_VALUE', 'build_policy')


def check(manifest: Path, lock: Path, archive: Path | None) -> None:
    document = load(manifest)
    validate_manifest(document)
    binding = load(lock)
    if binding.get('schema') != 'aa-dependency-lock-1':
        raise ManifestError('LOCK_MISMATCH', 'schema')
    manifest_hash = digest(binding.get('manifest_sha256'), 64, 'manifest_sha256')
    if hashlib.sha256(manifest.read_bytes()).hexdigest() != manifest_hash:
        raise ManifestError('LOCK_MISMATCH', str(manifest))
    commits = mapping(binding.get('source_commits'), 'source_commits')
    if commits != {source.name: source.commit for source in SOURCES}:
        raise ManifestError('LOCK_MISMATCH', 'source_commits')
    if binding.get('googletest_archive_sha256') != SOURCES[2].archive_sha256:
        raise ManifestError('LOCK_MISMATCH', 'googletest_archive_sha256')
    if binding.get('patch_path') != PATCH_PATH or binding.get('patch_sha256') != PATCH_SHA256:
        raise ManifestError('LOCK_MISMATCH', 'patch_binding')
    patch = manifest.parent / PATCH_PATH
    if hashlib.sha256(patch.read_bytes()).hexdigest() != PATCH_SHA256:
        raise ManifestError('PATCH_MISMATCH', str(patch))
    if archive is not None and hashlib.sha256(archive.read_bytes()).hexdigest() != SOURCES[2].archive_sha256:
        raise ManifestError('ARCHIVE_MISMATCH', str(archive))


def main() -> int:
    """CLI boundary: deterministic failure category; no writes or downloads."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--lock', type=Path, required=True)
    parser.add_argument('--archive', type=Path)
    args = parser.parse_args()
    lock: Path = args.lock
    manifest: Path = args.manifest or lock.with_name('manifest.json')
    archive: Path | None = args.archive
    try:
        check(manifest, lock, archive)
    except ManifestError as error:
        print(error)
        return 1
    except (OSError, json.JSONDecodeError, UnicodeDecodeError) as error:
        print(f'MALFORMED_VALUE\t{error}')
        return 1
    print('dependency-check: PASS source-pins=3 host-inventory=25 target=pending-task5 full-build=blocked')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
