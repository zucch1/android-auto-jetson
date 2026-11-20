#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Process exit stays bounded while a third-party FILE lock is held.

Each command runs with LD_PRELOAD=file_lock_holder.so, which leaves a thread
blocked in fwrite() holding its FILE lock. A process that ends through exit()
waits for that lock in glibc's stream cleanup. The receiver and the display
end with _Exit after their own bounded JSON shutdown (tools/json_stdout.hpp),
so each command must finish quickly with its normal status and JSON-only
stdout.
"""
import argparse
import json
import os
import subprocess
import sys
import time

BOUND_S = 10


def run(argv, shim, stdin=""):
    env = dict(os.environ, LD_PRELOAD=shim)
    began = time.monotonic()
    try:
        result = subprocess.run(argv, input=stdin, capture_output=True, text=True,
                                timeout=BOUND_S, env=env)
    except subprocess.TimeoutExpired as expired:
        raise AssertionError(("PROCESS_EXIT_BLOCKED_ON_FILE_LOCK", argv[1:])) from expired
    return result, time.monotonic() - began


def json_lines(stdout):
    lines = stdout.splitlines()
    assert lines, "no stdout"
    for line in lines:
        assert isinstance(json.loads(line), dict), line
    return lines


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--shim", required=True)
    parser.add_argument("--receiver")
    parser.add_argument("--display")
    args = parser.parse_args()
    cases = []
    if args.receiver:
        cases += [([args.receiver, "--list-video-capabilities"], "", 0, "json"),
                  ([args.receiver, "--no-such-flag"], "", 2, None)]
    if args.display:
        cases += [([args.display, "--list-video-capabilities"], "", 0, "json"),
                  ([args.display, "--help"], "", 0, "usage"),
                  ([args.display, "--no-such-flag"], "", 2, None),
                  ([args.display, "--test-only-control-stdin"], "quit\n", 0, "json")]
    assert cases, "nothing to run"
    for argv, stdin, code, output in cases:
        result, elapsed = run(argv, args.shim, stdin)
        assert result.returncode == code, (argv[1:], result.returncode, result.stderr[-400:])
        if output == "json":
            json_lines(result.stdout)
        elif output == "usage":
            assert result.stdout.startswith("usage: aa-dummy-display"), result.stdout[:200]
        print(f"ok {' '.join(argv[1:])} rc={result.returncode} {elapsed:.2f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
