# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# --- How to run ---
# python3 -B tools/sysroot/cli.py --help
# bash tools/sysroot/extract-jetson-sysroot.sh --snapshot SNAPSHOT --metadata METADATA --destination PARENT
"""Offline sysroot materialize, validate, and fixture-provenance guard commands."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from enum import StrEnum
import json
from pathlib import Path
import sys
from typing import assert_never

if __package__ in (None, ''):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.sysroot.models import Code, SysrootError, require_observed
from tools.sysroot.stage import materialize, validate_artifact


class Command(StrEnum):
    MATERIALIZE = 'materialize'
    VALIDATE = 'validate'
    REQUIRE_OBSERVED = 'require-observed'


@dataclass(frozen=True, slots=True)
class Arguments:
    command: Command
    snapshot: Path | None
    metadata: Path | None
    destination: Path | None
    artifact: Path | None


def arguments() -> Arguments:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    copy = commands.add_parser('materialize', help='Copy explicit local snapshot bytes')
    for option in ('snapshot', 'metadata', 'destination'):
        copy.add_argument('--' + option, type=Path, required=True)
    for command in ('validate', 'require-observed'):
        check = commands.add_parser(command, help='Validate artifact integrity; no authenticity claim')
        check.add_argument('--artifact', type=Path, required=True)
    args = parser.parse_args()
    return Arguments(Command(args.command), getattr(args, 'snapshot', None),
                     getattr(args, 'metadata', None), getattr(args, 'destination', None),
                     getattr(args, 'artifact', None))


def main() -> int:
    args = arguments()
    try:
        match args.command:
            case Command.MATERIALIZE:
                assert args.snapshot is not None and args.metadata is not None and args.destination is not None
                result = materialize(args.snapshot, args.metadata, args.destination)
            case Command.VALIDATE:
                assert args.artifact is not None
                result = validate_artifact(args.artifact)
            case Command.REQUIRE_OBSERVED:
                assert args.artifact is not None
                result = validate_artifact(args.artifact)
                require_observed(result.manifest)
            case unreachable:
                assert_never(unreachable)
    except SysrootError as error:
        print(json.dumps({'code': error.code, 'field': error.field}), file=sys.stderr)
        return 1
    except OSError as error:
        print(json.dumps({'code': Code.LOCAL_IO, 'errno': error.errno}), file=sys.stderr)
        return 1
    print(json.dumps({'artifact': str(result.artifact), 'payload_root': str(result.payload_root),
                      'manifest_sha256': result.manifest_sha256, 'reused': result.reused,
                      'provenance': result.manifest.provenance, 'capability': result.capability}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
