# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: tools/target/probe-wireless.sh --help
"""Host window orchestration; all target operations require an external window hook."""
import argparse
from dataclasses import asdict
import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.target_safety.kernel import Blocked
from tools.target_safety.snapshot_codec import encode_name
from tools.target_safety.snapshot_diff import diff_manifests, load_manifest
from wireless_target import Report


def invoke(args: list[str], payload: str | None = None) -> str:
    return subprocess.run(args, input=payload, capture_output=True, text=True,
                          timeout=900, check=True).stdout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jetson', required=True)
    parser.add_argument('--interface', default='wlan0')
    parser.add_argument('--jurisdiction-confirm', default='')
    parser.add_argument('--active', action='store_true')
    parser.add_argument('--ap-psk-file', type=Path)
    parser.add_argument('--duration', type=int, default=30)
    parser.add_argument('--window-id', default='')
    parser.add_argument('--window-hook', type=Path)
    parser.add_argument('--codegraph-root', default='')
    args = parser.parse_args()
    report = asdict(Report(target=args.jetson, confirmed_jurisdiction=args.jurisdiction_confirm))
    if (not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.@-]*', args.jetson) or
            not re.fullmatch(r'[A-Za-z0-9_-]+', args.interface) or
            (args.jurisdiction_confirm and not re.fullmatch(r'[A-Z]{2}', args.jurisdiction_confirm)) or
            not 1 <= args.duration <= 120):
        report['blockers'].append('invalid-arguments')
        print(json.dumps(report))
        return 1
    if not args.window_hook or not args.window_id or not args.codegraph_root:
        report['blockers'].append('authorized-inventory-window-required')
        print(json.dumps(report))
        return 1
    hook = [str(args.window_hook.resolve()), args.window_id, args.jetson]
    before: Path | None = None
    inventory: dict[str, str] = {}
    try:
        # Hook must perform the first protected traversal before SSH preflight/discovery.
        before = Path(invoke(hook + ['before']).strip())
        if not before.is_absolute():
            raise OSError('absolute-host-inventory-path-required')
        manifest = load_manifest(before)
        expected = [list(encode_name(path.encode())) for path in
                    ('/home/zucchi/Desktop/infotainment-plan', args.codegraph_root)]
        if (manifest['target_identity'] != args.jetson or
                manifest['task_window_id'] != args.window_id or
                manifest['roots'] != expected):
            raise OSError('inventory-window-binding-failed')
        inventory['before'] = str(before)
        ssh = ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10',
               '-o', 'StrictHostKeyChecking=yes', args.jetson]
        invoke(ssh + ['true'])
        psk = args.ap_psk_file.read_text().strip() if args.ap_psk_file else ''
        if args.active and not 8 <= len(psk) <= 63:
            raise OSError('active-phase-requires-8-to-63-character-psk-file')
        argv = ['wireless_target', '--target', args.jetson, '--interface', args.interface,
                '--jurisdiction-confirm', args.jurisdiction_confirm, '--duration', str(args.duration)]
        if args.active:
            argv += ['--active', '--ap-psk', psk]
        here = Path(__file__).parent
        # Sources and PSK travel through SSH stdin; no target source files or PSK argv.
        payload = ('import sys, types\n'
                   "module = types.ModuleType('wireless_bluez')\n"
                   "sys.modules['wireless_bluez'] = module\n"
                   f'exec({(here / "wireless_bluez.py").read_text()!r}, module.__dict__)\n'
                   f'sys.argv = {argv!r}\n'
                   f'exec({(here / "wireless_target.py").read_text()!r})\n')
        result = subprocess.run(ssh + [shlex.join(['python3', '-B', '-'])], input=payload,
                                capture_output=True, text=True, timeout=args.duration + 600, check=False)
        observed = json.loads(result.stdout)
        if (observed.keys() != report.keys() or observed['schema'] != report['schema'] or
                observed['target'] != args.jetson):
            raise OSError('invalid-target-report')
        report = observed
        if result.returncode and not report['blockers']:
            report['blockers'].append('target-probe-failed')
    except (OSError, subprocess.SubprocessError, ValueError, Blocked) as error:
        report['blockers'].append('window-or-capability-operation-failed')
        # Do not serialize SubprocessError commands (may include test credentials).
        report['raw']['host_error_type'] = type(error).__name__
    finally:
        if before is not None:
            try:
                invoke(hook + ['workload-cleanup'])
                after = Path(invoke(hook + ['after']).strip())
                if not after.is_absolute():
                    raise OSError('absolute-host-inventory-path-required')
                closing = load_manifest(after)
                if closing['task_window_id'] != args.window_id:
                    raise OSError('after-inventory-window-binding-failed')
                diff = before.with_name(before.stem + '-wireless-diff.json')
                verdict = diff_manifests(before, after, diff)
                inventory.update(after=str(after), diff=str(diff), verdict=verdict.verdict)
                if verdict.exit_status:
                    report['blockers'].append('protected-inventory-not-equivalent')
            except (OSError, subprocess.SubprocessError, Blocked) as error:
                report['blockers'].append('closing-inventory-failed')
                report['raw']['closing_error_type'] = type(error).__name__
        try:
            invoke(hook + ['final-cleanup'])
        except (OSError, subprocess.SubprocessError):
            report['blockers'].append('final-window-cleanup-failed')
    report['protected_inventory'] = inventory
    print(json.dumps(report, sort_keys=True))
    return int(bool(report['blockers']))


if __name__ == '__main__':
    sys.exit(main())
