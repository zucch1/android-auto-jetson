# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression test for the sandbox in tools/ci/arm64-cross-relay.sh (security finding H1).

Runs the real relay script in its test mode (no network fetch, no real sysroot validator or
cmake build; the sandboxed step is a probe command) with a fake `gh` first on PATH, and proves:

* the sandboxed step sees none of the owner's HOME, gh config, SSH agent or token variables,
  has only the scrubbed environment, no network (not even host loopback), a read-only
  sysroot, and no .git link back to the owner's repository;
* the trusted step runs from the base checkout with a scrubbed environment;
* the status post runs OUTSIDE the sandbox and only AFTER the sandbox exited, keyed to the
  SHA, with state following the sandbox exit code;
* a non-owner PR author is refused before any untrusted code runs or any status is posted;
* with no usable backend the selector fails closed.

Exits 77 (ctest SKIP) with a reason when no sandbox backend works on this machine.
"""
import os
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RELAY = ROOT / 'tools' / 'ci' / 'arm64-cross-relay.sh'
LIB = ROOT / 'tools' / 'ci' / 'relay-sandbox-lib.sh'
SKIP = 77
SECRET = 'ghp_SECRET_MUST_NOT_REACH_THE_SANDBOX'


def skip(reason: str) -> None:
    print(f'SKIP: {reason}')
    raise SystemExit(SKIP)


def detect_backend() -> str:
    probe = subprocess.run(
        ['bash', '-c', f'. {shlex.quote(str(LIB))}; relay_sandbox_select auto'],
        capture_output=True, text=True, check=False)
    if probe.returncode != 0:
        skip('no usable sandbox backend (needs bwrap with unprivileged user namespaces, or '
             'podman/docker with AA_RELAY_IMAGE set to a local image): ' + probe.stderr.strip().splitlines()[0])
    return probe.stdout.strip()


FAKE_GH = r'''#!/bin/sh
# Fake gh: records where it ran; never talks to GitHub.
if [ "$1" = pr ] && [ "$2" = view ]; then
    echo "$FAKE_PR_AUTHOR $FAKE_PR_HEAD"
    exit 0
fi
{
    echo "args=$*"
    echo "home=$HOME"
    echo "outside_marker_visible=$( [ -e "$OUTSIDE_MARKER" ] && echo yes || echo no )"
    echo "gh_token_visible=$( [ -n "${GH_TOKEN:-}" ] && echo yes || echo no )"
} >> "$GH_LOG"
echo "GH-CALL $*" >&2
exit 0
'''

# Runs inside the sandbox via `sh -c`; the outside marker path is spliced in by the test.
PROBE = r'''
fail() { echo "SANDBOX-LEAK: $*"; exit 1; }
[ "$HOME" = /home/aa-relay ] || fail "HOME=$HOME"
[ -z "${GH_TOKEN:-}${GITHUB_TOKEN:-}${SSH_AUTH_SOCK:-}" ] || fail "credential variable present"
[ ! -e "$HOME/.config/gh" ] || fail "gh config visible"
[ ! -e @OUTSIDE@ ] || fail "owner-side marker visible"
[ ! -e .git ] || fail ".git present in exported tree"
[ "$GIT_CONFIG_NOSYSTEM" = 1 ] || fail "GIT_CONFIG_NOSYSTEM"
if ( : > "$AA_SYSROOT_ROOTFS/probe-write" ) 2>/dev/null; then fail "sysroot is writable"; fi
if ( : >> "$AA_GOOGLETEST_ARCHIVE" ) 2>/dev/null; then fail "archive is writable"; fi
if ( : >> "$AA_SYSROOT_MANIFEST" ) 2>/dev/null; then fail "manifest is writable"; fi
: > "$AA_SYSROOT_CACHE/ok" || fail "cache not writable"
: > build-output || fail "tree not writable"
python3 - <<'PY' || exit 1
import socket, sys
for host, port in (('127.0.0.1', @PORT@), ('93.184.216.34', 443)):
    s = socket.socket()
    s.settimeout(2)
    try:
        s.connect((host, port))
    except OSError:
        continue
    print(f'SANDBOX-LEAK: network reachable {host}:{port}')
    sys.exit(1)
PY
nics=$(ls /sys/class/net 2>/dev/null | grep -v '^lo$' || true)
[ -z "$nics" ] || fail "network interfaces: $nics"
env | sort
echo SANDBOX-OK
'''


def run_relay(tmp: Path, sha: str, *, probe: str, pr: str | None = None, author: str = 'zucch1',
              extra: tuple[str, ...] = (), backend: str) -> tuple[int, str, str]:
    scratch = tmp / 'scratch'
    outside = tmp / 'outside-marker'
    outside.write_text('owner-only\n', encoding='utf-8')
    home = tmp / 'home'
    (home / '.config' / 'gh').mkdir(parents=True, exist_ok=True)
    (home / '.config' / 'gh' / 'hosts.yml').write_text(f'oauth_token: {SECRET}\n', encoding='utf-8')
    fakebin = tmp / 'bin'
    fakebin.mkdir(exist_ok=True)
    gh = fakebin / 'gh'
    gh.write_text(FAKE_GH, encoding='utf-8')
    gh.chmod(0o755)
    sysroot = tmp / 'sysroot' / 'rootfs'
    sysroot.mkdir(parents=True, exist_ok=True)
    manifest = tmp / 'sysroot' / 'manifest.json'
    manifest.write_text('{}\n', encoding='utf-8')
    archive = tmp / 'googletest.tar.gz'
    archive.write_bytes(b'x')
    gh_log = tmp / 'gh.log'
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    listener.setblocking(False)
    port = listener.getsockname()[1]
    env = {
        'PATH': f'{fakebin}:{os.environ["PATH"]}',
        'HOME': str(home),
        'GH_TOKEN': SECRET,
        'GITHUB_TOKEN': SECRET,
        'SSH_AUTH_SOCK': str(tmp / 'agent.sock'),
        'AA_SYSROOT_ROOTFS': str(sysroot),
        'AA_SYSROOT_MANIFEST': str(manifest),
        'AA_GOOGLETEST_ARCHIVE': str(archive),
        'AA_RELAY_SCRATCH': str(scratch),
        'AA_RELAY_TEST_MODE': '1',
        'AA_RELAY_SANDBOX': backend,
        'AA_RELAY_TEST_TRUSTED_CMD': 'echo "TRUSTED-CWD=$(pwd)"; echo "TRUSTED-HOME=$HOME"; '
                                     '[ -z "${GH_TOKEN:-}${SSH_AUTH_SOCK:-}" ] || { echo TRUSTED-LEAK; exit 1; }; '
                                     'echo TRUSTED-OK',
        'AA_RELAY_TEST_SANDBOX_CMD': probe.replace('@OUTSIDE@', shlex.quote(str(outside))).replace('@PORT@', str(port)),
        'GH_LOG': str(gh_log),
        'OUTSIDE_MARKER': str(outside),
        'FAKE_PR_AUTHOR': author,
        'FAKE_PR_HEAD': sha,
    }
    if 'AA_RELAY_IMAGE' in os.environ:
        env['AA_RELAY_IMAGE'] = os.environ['AA_RELAY_IMAGE']
    args = ['bash', str(RELAY), *extra, sha] + ([pr] if pr else [])
    done = subprocess.run(args, env=env, capture_output=True, text=True, timeout=120, check=False)
    try:
        listener.accept()
        raise AssertionError('the sandbox reached the host loopback listener')
    except BlockingIOError:
        pass
    finally:
        listener.close()
    log = gh_log.read_text(encoding='utf-8') if gh_log.exists() else ''
    return done.returncode, done.stderr, log


def read_step_log(tmp: Path, name: str) -> str:
    return (tmp / 'scratch' / 'logs' / f'{name}.log').read_text(encoding='utf-8')


def check(condition: bool, message: str, detail: str = '') -> None:
    if not condition:
        print(f'FAIL: {message}\n{detail}')
        raise SystemExit(1)


def main() -> int:
    backend = detect_backend()
    sha = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                         check=True).stdout.strip()
    print(f'sandbox backend under test: {backend}')

    # 1. Passing sandbox: isolation holds, status posted after the sandbox, outside it.
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        rc, err, gh_log = run_relay(tmp, sha, probe=PROBE, backend=backend)
        sandbox_log = read_step_log(tmp, '10-sandbox-probe')
        check(rc == 0, 'relay with a passing probe must exit 0', err + sandbox_log)
        check('SANDBOX-OK' in sandbox_log and 'SANDBOX-LEAK' not in sandbox_log,
              'sandbox probe did not pass', sandbox_log)
        check(SECRET not in sandbox_log, 'token leaked into the sandbox environment', sandbox_log)
        allowed = {'HOME', 'PATH', 'GIT_CONFIG_NOSYSTEM', 'PKG_CONFIG_PATH', 'PWD', 'SHLVL', '_', 'OLDPWD',
                   'AA_SYSROOT_ROOTFS', 'AA_SYSROOT_MANIFEST', 'AA_GOOGLETEST_ARCHIVE', 'AA_SYSROOT_CACHE',
                   'AA_SYSROOT_PAYLOAD', 'AA_SYSROOT_REQUIRE_OBSERVED',
                   # image-defined variables of the container backends
                   'container', 'HOSTNAME', 'LANG', 'LC_ALL', 'DEBIAN_FRONTEND'}
        names = {line.split('=', 1)[0] for line in sandbox_log.splitlines() if '=' in line and line[:1].isupper()}
        check(names <= allowed, f'unexpected sandbox environment variables: {sorted(names - allowed)}', sandbox_log)
        trusted_log = read_step_log(tmp, '00-require-observed')
        check('TRUSTED-OK' in trusted_log, 'trusted step failed', trusted_log)
        cwd = next(line for line in trusted_log.splitlines() if line.startswith('TRUSTED-CWD=')).split('=', 1)[1]
        check(cwd.endswith('/scratch/base'), 'trusted step must run from the base checkout', trusted_log)
        check('SANDBOX_OK' not in trusted_log and 'pr-tree' not in cwd, 'trusted step ran in the PR tree', trusted_log)
        # Status post: exactly once, outside the sandbox, after the sandbox exited, keyed to the SHA.
        check(gh_log.count('args=') == 1, 'expected exactly one gh status post', gh_log)
        check(f'statuses/{sha}' in gh_log and 'state=success' in gh_log and 'context=ci/arm64-cross-build' in gh_log,
              'status post is not keyed to the SHA / success / required context', gh_log)
        check('outside_marker_visible=yes' in gh_log and f'home={tmp / "home"}' in gh_log,
              'gh must run in the owner environment, outside the sandbox', gh_log)
        exited = err.index('sandbox exited step=10-sandbox-probe rc=0')
        posted = err.index('GH-CALL')
        check(exited < posted, 'the status was posted before the sandbox exited', err)
        check(not (tmp / 'scratch' / 'pr-tree').exists(), 'untrusted PR tree was not cleaned up')
        check(not (tmp / 'sysroot' / 'rootfs' / 'probe-write').exists()
              and (tmp / 'googletest.tar.gz').read_bytes() == b'x'
              and (tmp / 'sysroot' / 'manifest.json').read_text(encoding='utf-8') == '{}\n',
              'the sysroot, manifest or archive was modified from inside the sandbox')

    # 2. A failing sandboxed build yields a failure status (the exit code decides, not the log text).
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        rc, err, gh_log = run_relay(tmp, sha, probe='echo SANDBOX-OK; exit 3', backend=backend)
        check(rc == 1, 'relay must exit non-zero when the sandboxed step fails', err)
        check('state=failure' in gh_log and 'state=success' not in gh_log, 'failure must be posted as failure', gh_log)

    # 3. Non-owner PR author: refused before any untrusted code runs, nothing posted.
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        rc, err, gh_log = run_relay(tmp, sha, probe=PROBE, pr='7', author='someone-else', backend=backend)
        check(rc == 65 and 'someone-else' in err, 'non-owner author must be refused', err)
        check(gh_log == '' and not (tmp / 'scratch' / 'logs' / '10-sandbox-probe.log').exists(),
              'refused PR must not run the sandbox or post a status', gh_log)
        rc, err, gh_log = run_relay(tmp, sha, probe='true', pr='7', author='someone-else',
                                    extra=('--allow-author', 'someone-else'), backend=backend)
        check(rc == 0, '--allow-author must admit the named author', err)

    # 4. Fail closed: with no backend on PATH the selector refuses (no unsandboxed fallback).
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        done = subprocess.run([shutil.which('bash'), '-c', f'. {shlex.quote(str(LIB))}; relay_sandbox_select auto'],
                              env={'PATH': t}, capture_output=True, text=True, check=False)
        check(done.returncode != 0 and 'unsandboxed' in done.stderr, 'selector must fail closed', done.stderr)

    print('relay sandbox test: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
