# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tools/build/contamination.py --binary build/jetson-aarch64/aa_cross_smoke --sysroot .local/sysroots/jetson-r39.2.1/rootfs
"""Host-contamination QA: a clean cross binary references aarch64 sysroot libs, never host /usr."""
from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import json
from pathlib import Path
import re
import subprocess
import sys

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.models import Code, SysrootError
from tools.build.input_provenance import Inputs, check_inputs
from tools.build.decode_overlay import Overlay

AARCH64_INTERP = '/lib/ld-linux-aarch64.so.1'
BANNED_HOST = (
    '/usr/lib/x86_64-linux-gnu', '/lib/x86_64-linux-gnu', '/usr/lib64', '/lib64/ld-linux-x86-64',
    '/usr/lib/i386-linux-gnu', 'ld-linux-x86-64',
)


@dataclass(frozen=True, slots=True)
class Report:
    machine: str
    interpreter: str
    needed: tuple[str, ...]
    rpath: tuple[str, ...]
    contamination: tuple[str, ...]
    clean: bool


def _readelf(binary: Path, *flags: str) -> str:
    result = subprocess.run(['readelf', *flags, str(binary)], capture_output=True, text=True,
                            check=False)
    if result.returncode != 0:
        raise SysrootError(Code.LOCAL_IO, f'readelf {binary}')
    return result.stdout


def machine(binary: Path) -> str:
    match = re.search(r'Machine:\s+(.+)', _readelf(binary, '-h'))
    return match.group(1).strip() if match else ''


def interpreter(binary: Path) -> str:
    match = re.search(r'\[Requesting program interpreter: ([^\]]+)\]', _readelf(binary, '-l'))
    return match.group(1).strip() if match else ''


def dynamic_strings(binary: Path) -> tuple[str, ...]:
    text = _readelf(binary, '-d')
    return tuple(re.findall(r'\((?:NEEDED|RPATH|RUNPATH)\)[^\[]*\[([^\]]+)\]', text))


def check(binary: Path) -> Report:
    found_machine = machine(binary)
    interp = interpreter(binary)
    strings = dynamic_strings(binary)
    needed = tuple(s for s in strings if not s.startswith('/'))
    rpath = tuple(s for s in strings if s.startswith('/'))
    contamination: list[str] = []
    if 'AArch64' not in found_machine:
        contamination.append(f'machine:{found_machine or "unknown"}')
    if interp != AARCH64_INTERP:
        contamination.append(f'interpreter:{interp or "none"}')
    for value in strings:
        for banned in BANNED_HOST:
            if banned in value:
                contamination.append(f'host-path:{value}')
                break
    return Report(found_machine, interp, needed, rpath, tuple(contamination),
                  not contamination)


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--sysroot', type=Path)
    parser.add_argument('--build', type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--compiler', type=Path)
    parser.add_argument('--manifest', type=Path)
    parser.add_argument('--payload', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--overlay', type=Path)
    parser.add_argument('--overlay-manifest', type=Path)
    parser.add_argument('--overlay-digest')
    return parser.parse_args()


def main() -> int:
    args = arguments()
    try:
        overlay = None
        if any(value is not None for value in (args.overlay, args.overlay_manifest, args.overlay_digest)):
            if args.overlay is None or args.overlay_manifest is None or args.overlay_digest is None or args.sysroot is None:
                raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.overlay-arguments')
            overlay = Overlay(args.overlay.resolve(strict=True), args.overlay_manifest, args.overlay_digest)
        report = check(args.binary)
        document = asdict(report)
        document['acceptance'] = 'elf-only-not-input-provenance'
        if args.sysroot is not None:
            if args.build is None or args.source is None or args.compiler is None:
                raise SysrootError(Code.ARTIFACT_MISMATCH, 'provenance.arguments')
            inputs = Inputs(args.sysroot, args.build, args.source, args.compiler,
                            args.manifest, args.payload, overlay)
            provenance = check_inputs(args.binary, inputs)
            document['inputs'] = asdict(provenance)
            if provenance.overlay is not None:
                document['inputs']['overlay'] = {
                    'root': str(provenance.overlay.root),
                    'manifest': str(provenance.overlay.manifest),
                    'digest': provenance.overlay.digest,
                }
            document['acceptance'] = provenance.acceptance
            document['clean'] = report.clean and not provenance.contamination
    except SysrootError as error:
        print(json.dumps({'code': error.code, 'field': error.field}), file=sys.stderr)
        return 1
    except (OSError, subprocess.SubprocessError) as error:
        print(json.dumps({'code': Code.LOCAL_IO, 'field': str(error)}), file=sys.stderr)
        return 1
    document['code'] = None if document['clean'] else Code.ARTIFACT_MISMATCH
    if args.output is not None:
        args.output.write_text(json.dumps(document, indent=2) + '\n')
    print(json.dumps(document))
    return 0 if document['clean'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
