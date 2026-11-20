#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Stdout isolation fails closed (tools/json_stdout.hpp).

Each command starts with fd 1 closed, so the real stdout cannot be moved to a
private descriptor. Running anyway would put library text and JSON on
whatever later reuses fd 1. The tool must refuse before loading any library:
exit status 4, one stdout_isolation_failed line with the errno on stderr, no
JSON on stderr, and nothing created in its runtime directory.
"""
import argparse
import os
import subprocess
import sys
import tempfile

BOUND_S = 10
EXPECTED_CODE = 4


def close_stdout():
    os.close(1)


def check(program, argv, runtime, stdin=""):
    result = subprocess.run(argv, input=stdin, stdout=None, stderr=subprocess.PIPE, text=True,
                            timeout=BOUND_S, preexec_fn=close_stdout)
    where = (argv[1:], result.returncode, result.stderr[-400:])
    assert result.returncode == EXPECTED_CODE, where
    lines = result.stderr.splitlines()
    expected = f"{program}: stdout_isolation_failed: {os.strerror(9)} (errno 9)"
    assert expected in lines, (where, expected)
    assert not any(line.lstrip().startswith("{") for line in lines), where
    assert os.listdir(runtime) == [], (where, os.listdir(runtime))
    print(f"ok {program} {' '.join(argv[1:])} rc={result.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--receiver")
    parser.add_argument("--display")
    args = parser.parse_args()
    assert args.receiver or args.display, "nothing to run"
    with tempfile.TemporaryDirectory(prefix="aa-isolation-") as runtime:
        if args.receiver:
            check("aa-wired-receiver", [args.receiver, "--list-video-capabilities"], runtime)
            check("aa-wired-receiver",
                  [args.receiver, "--port", "1-2.1", "--no-bus", "--duration-ms", "200",
                   "--store", os.path.join(runtime, "approved.json"),
                   "--video-socket", os.path.join(runtime, "video"),
                   "--input-socket", os.path.join(runtime, "input")], runtime)
        if args.display:
            check("aa-dummy-display", [args.display, "--list-video-capabilities"], runtime)
            check("aa-dummy-display", [args.display, "--decoder", "auto", "--check-pipeline"], runtime)
            check("aa-dummy-display",
                  [args.display, "--test-only-control-stdin",
                   "--video-socket", os.path.join(runtime, "video"),
                   "--input-socket", os.path.join(runtime, "input")], runtime, "quit\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
