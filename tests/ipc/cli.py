# SPDX-License-Identifier: GPL-3.0-or-later
import json
import subprocess
import sys


def main() -> int:
    binary = sys.argv[1]
    for args in [('--duration', '0'), ('--duration', '3601'), ('--duration', '-1'),
                 ('--duration', '2x'), ('--duration', '99999999999999999'),
                 ('--profile', '800x480@30'), ('--duration',), ('--unknown', 'x')]:
        result = subprocess.run([binary, *args], capture_output=True, timeout=5, check=False)
        if result.returncode != 2:
            raise RuntimeError(f'invalid CLI accepted: {args}: {result.returncode}')
    result = subprocess.run([binary, '--profile', '1280x720@30', '--duration', '2'],
                            capture_output=True, text=True, timeout=10, check=False)
    if result.returncode != 0:
        raise RuntimeError(f'benchmark failed: {result.stdout} {result.stderr}')
    report = json.loads(result.stdout)
    if not (report['pass'] and report['frames_delivered'] == 60 and
            report['fragmented_idrs'] == 2 and report['throughput_mbps'] >= 10 and
            report['p95_added_latency_ms'] <= 33 and report['bounded_queue'] and
            report['induced_loss']['clean_idr_resync'] and report['induced_loss']['drops'] > 0):
        raise RuntimeError(f'wrong benchmark report: {report}')
    print('ipc CLI and measured report PASS')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
