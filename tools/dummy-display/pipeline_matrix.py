#!/usr/bin/env python3
"""Probe every pipeline, smoke-run available combinations; no decode qualification claim."""
import argparse
import itertools
import json
import os
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--display', required=True)
    args = parser.parse_args()
    env = dict(os.environ, QT_QPA_PLATFORM=os.environ.get('QT_QPA_PLATFORM', 'offscreen'))
    ran = failed = 0
    for decoder, presentation, thread in itertools.product(
            ('software', 'hardware', 'auto'), ('cpu', 'gpu', 'zerocopy'), ('gui', 'worker')):
        command = [args.display, '--decoder', decoder, '--presentation', presentation, '--media-thread', thread]
        probe = subprocess.run(command + ['--check-pipeline'], capture_output=True, text=True, env=env, timeout=15)
        result = dict(decoder=decoder, presentation=presentation, media_thread=thread)
        if probe.returncode == 0:
            run = subprocess.run(command + ['--test-only-control-stdin'], input='quit\n', capture_output=True, text=True, env=env, timeout=15)
            ok = run.returncode == 0 and '"cmd":"quit"' in run.stdout
            result.update(status='ran' if ok else 'failed', detail=run.stderr)
            ran += ok
            failed += not ok
        else:
            result.update(status='unavailable', detail=probe.stderr.strip())
        print(json.dumps(result))
    print(json.dumps(dict(ran=ran, unavailable=18-ran-failed, failed=failed)))
    return int(failed != 0 or ran == 0)


if __name__ == '__main__':
    raise SystemExit(main())
