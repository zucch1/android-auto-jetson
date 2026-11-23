# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
# Run: python3 -B tools/qualification/check_wired.py PACKAGE/wired.json
"""Check a wired evidence export; success is not independent hardware attestation."""
from __future__ import annotations

import argparse
from pathlib import Path

from check_privacy import scan
from common import EvidenceError, artifact, load, package_file, require
from results import check_performance_runs, load_performance, performance, projection, provenance, recovery


def check(manifest: Path) -> None:
    root = manifest.absolute().parent
    scan(root)
    doc = load(package_file(root, manifest.name))
    doc.keys({'schema', 'target', 'phone', 'usb_topology', 'session_trace', 'redacted_log',
              'task30', 'task31', 'perf_30m', 'soak_2h', 'limitations', 'provenance'})
    doc.equal('schema', 'aa-wired-qualification/1')
    for section, fields in (
        ('target', {'model', 'jetpack_version', 'l4t_version', 'kernel_version'}),
        ('phone', {'model', 'android_version', 'android_auto_version'}),
        ('usb_topology', {'controller', 'port', 'hub', 'cable'}),
    ):
        child = doc.child(section, fields=fields)
        for field in fields:
            child.text(field)
    limitations = doc.array('limitations')
    require(bool(limitations) and all(isinstance(item, str) and bool(item.strip())
                                    for item in limitations), 'MISSING_LIMITATIONS')
    provenance(doc.child('provenance'))
    for name in ('session_trace', 'redacted_log'):
        artifact(root, doc.child(name))
    paths = {name: artifact(root, doc.child(name))
             for name in ('task30', 'task31', 'perf_30m', 'soak_2h')}
    require(len(set(paths.values())) == 4, 'RESULTS_MUST_BE_SEPARATE')
    projection_result = load(paths['task30'])
    recovery_result = load(paths['task31'])
    for result in (projection_result, recovery_result):
        result.equal('port', doc.child('usb_topology').text('port'))
    projection(projection_result)
    recovery(recovery_result)
    short = load_performance(paths['perf_30m'])
    long = load_performance(paths['soak_2h'])
    performance(short, 1800)
    performance(long, 7200)
    check_performance_runs(short, long, doc)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('manifest', type=Path)
    args = parser.parse_args()
    try:
        check(args.manifest)
    except (EvidenceError, OSError, UnicodeError, ValueError, RecursionError):
        print('wired-check: FAIL incomplete, invalid or unsafe package (content withheld)')
        return 1
    print('wired-check: PASS evidence contract only; package equivalence remains provisional')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
