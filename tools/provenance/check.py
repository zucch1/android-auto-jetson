# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/provenance/check.py [--root ROOT]
"""Check retained source bytes against immutable original Git blob identities."""
import argparse
from pathlib import Path
import re
import stat
from typing import Final

from inventory import SOURCES, Entry, ProvenanceError, Source, blob_oid, entries, render, sha256

GPL_SHA256: Final = '3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986'
GENERATED: Final = re.compile(
    r'\.pb\.(?:cc|h|go|py)$|_pb2\.py$|\.(?:o|a|so|pyc)$|'
    r'(?:^|/)(?:CMakeFiles|__pycache__)(?:/|$)|'
    r'(?:^|/)(?:CMakeCache\.txt|compile_commands\.json)$'
)
NOTICE_FACTS: Final = (
    'aasdk-root-license-missing',
    'fallback-f1xpl-aasdk-9ee5283245a630afce3a441696b9dde484ecbcd9',
    'headunit-credential-public-reference',
    'oaa-reference-only',
)


def source_gate(root: Path, source: Source, original: tuple[Entry, ...]) -> list[ProvenanceError]:
    """Check exact file set, regular-file type, mode, size and raw blob ID."""
    failures: list[ProvenanceError] = []
    vendor = root / source.destination
    # Reject directory links too, including ancestors of the source root.
    ancestors = (vendor, *vendor.parents)
    for path in ancestors:
        if path == root:
            break
        if path.is_symlink():
            return [ProvenanceError('SYMLINK_REJECTED', str(path.relative_to(root)))]
    expected = {entry.path: entry for entry in original if source.includes(entry.path)}
    oaa = SOURCES[1]
    oaa_entries = entries(root, oaa)
    oaa_paths = {entry.path for entry in oaa_entries if oaa.includes(entry.path)}
    oaa_oids = {entry.oid for entry in oaa_entries if oaa.includes(entry.path)}
    actual: set[str] = set()
    for path in sorted(vendor.rglob('*')):
        relative = path.relative_to(vendor).as_posix()
        diagnostic_path = path.relative_to(root).as_posix()
        if path.is_symlink():
            failures.append(ProvenanceError('SYMLINK_REJECTED', diagnostic_path))
            continue
        mode = path.stat().st_mode
        if stat.S_ISDIR(mode):
            continue
        actual.add(relative)
        if not stat.S_ISREG(mode):
            failures.append(ProvenanceError('NON_REGULAR_FILE', diagnostic_path))
            continue
        data = path.read_bytes()
        oid = blob_oid(data)
        entry = expected.get(relative)
        if entry is None:
            code = 'EXTRA_VENDORED_FILE'
            if source.name == 'aasdk' and (relative in oaa_paths or oid in oaa_oids):
                code = 'OAA_OVERLAY_IN_AASDK'
            elif source.name == 'aasdk' and GENERATED.search(relative):
                code = 'GENERATED_OUTPUT_IN_AASDK'
            failures.append(ProvenanceError(code, diagnostic_path))
            continue
        if oid != entry.oid:
            failures.append(ProvenanceError('BLOB_MISMATCH', diagnostic_path))
        if len(data) != entry.size:
            failures.append(ProvenanceError('SIZE_MISMATCH', diagnostic_path))
        git_mode = '100755' if mode & stat.S_IXUSR else '100644'
        if git_mode != entry.mode:
            failures.append(ProvenanceError('MODE_MISMATCH', diagnostic_path))
    for relative in sorted(expected.keys() - actual):
        failures.append(ProvenanceError('MISSING_VENDORED_FILE', f'{source.destination}/{relative}'))
    return failures


def check(root: Path) -> list[ProvenanceError]:
    """Run source, license-fact and derived-report gates without modifying files."""
    failures: list[ProvenanceError] = []
    # Anchor both listings before inspecting vendor bytes or inventory data.
    originals = tuple(entries(root, source) for source in SOURCES)
    for source, original in zip(SOURCES, originals, strict=True):
        failures.extend(source_gate(root, source, original))
    invented = root / 'third_party/aasdk/LICENSE'
    if invented.exists() or invented.is_symlink():
        failures.append(ProvenanceError('INVENTED_LICENSE_PRESENT', 'third_party/aasdk/LICENSE'))
    gpl = root / 'third_party/LICENSES/GPL-3.0.txt'
    if gpl.is_symlink():
        failures.append(ProvenanceError('SYMLINK_REJECTED', str(gpl.relative_to(root))))
    if not gpl.is_file() or sha256(gpl.read_bytes()) != GPL_SHA256:
        failures.append(ProvenanceError('GPL_TEXT_MISMATCH', str(gpl.relative_to(root))))
    notices = root / 'THIRD_PARTY_NOTICES.md'
    if notices.is_file():
        facts = {line.removeprefix('- provenance-fact: ').strip()
                 for line in notices.read_text(encoding='utf-8').splitlines()
                 if line.startswith('- provenance-fact: ')}
        for fact in NOTICE_FACTS:
            if fact not in facts:
                failures.append(ProvenanceError('MISSING_LICENSE_FACT', fact))
    else:
        failures.append(ProvenanceError('MISSING_LICENSE_FACT', 'THIRD_PARTY_NOTICES.md'))
    if not (root / 'PROVENANCE.md').is_file():
        failures.append(ProvenanceError('MISSING_PROVENANCE', 'PROVENANCE.md'))
    if failures:
        return failures
    report = root / 'third_party/provenance/inventory.json'
    if report.is_symlink():
        return [ProvenanceError('SYMLINK_REJECTED', str(report.relative_to(root)))]
    if not report.is_file() or report.read_bytes() != render(root).encode('utf-8'):
        failures.append(ProvenanceError('INVENTORY_MISMATCH', str(report.relative_to(root))))
    return failures


def main() -> int:
    """CLI boundary: stable stdout diagnostics; zero clean, one on gate failure."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    root: Path = args.root.resolve()
    try:
        failures = check(root)
    except ProvenanceError as error:
        failures = [error]
    except OSError as error:
        failures = [ProvenanceError('IO_ERROR', str(error.filename))]
    for error in failures:
        print(error)
    if failures:
        print(f'provenance-check: FAIL violations={len(failures)}')
        return 1
    print('provenance-check: PASS aasdk=566 oaa=250 original-blobs=headers-retained')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
