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
* with no usable backend the selector fails closed;
* review fixes (PR #92): inherited file, directory and socket fds never reach the sandbox and
  stdin is /dev/null; the ORIGINAL credential path is absent, not just the replacement HOME; a
  writable mount that overlaps a read-only input (cache == parent of the sysroot, a cache inside
  the sysroot, or a symlink to it) is refused, and read-only mounts are emitted after writable
  ones; round 2: HOME, the repo root and its git dirs, logs, the base checkout and the empty
  HOME cannot be equalled or contained by a writable mount, and the default scratch is a fresh
  0700 mktemp directory; round 3: the repo root is fully protected, AA_SYSROOT_CACHE and an explicit
  AA_RELAY_SCRATCH must be nonexistent, empty or marked (an existing scratch also owned by the user and
  not group/world-writable), the default scratch lives under TMPDIR / /var/tmp and early exits leak
  nothing; a container that ignores SIGTERM is gone after the step timeout, and when its
  termination cannot be confirmed nothing is posted and the relay exits non-zero.

Exits 77 (ctest SKIP) with a reason when no sandbox backend works on this machine.
"""
import os
import shlex
import shutil
import socket
import signal
import stat
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RELAY = ROOT / 'tools' / 'ci' / 'arm64-cross-relay.sh'
LIB = ROOT / 'tools' / 'ci' / 'relay-sandbox-lib.sh'
SKIP = 77
# A canary standing in for the owner's gh credential. Assembled at runtime and deliberately not
# credential-shaped, so no scanner (CodeQL, gitleaks) sees a literal in the source. Its value is
# never printed: check() redacts it from every detail.
SANDBOX_CANARY = '-'.join(('canary', 'owner', 'credential', 'must', 'not', 'reach', 'the', 'sandbox'))


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
[ ! -e @ORIG_GH@ ] || fail "ORIGINAL gh config path visible"
[ ! -e @ORIG_HOME@ ] || fail "ORIGINAL owner HOME visible"
[ "$(readlink /proc/self/fd/0)" = /dev/null ] || fail "stdin is not /dev/null"
[ ! -e @OUTSIDE@ ] || fail "owner-side marker visible"
[ ! -e .git ] || fail ".git present in exported tree"
[ "$GIT_CONFIG_NOSYSTEM" = 1 ] || fail "GIT_CONFIG_NOSYSTEM"
if ( : > "$AA_SYSROOT_ROOTFS/probe-write" ) 2>/dev/null; then fail "sysroot is writable"; fi
if ( : >> "$AA_GOOGLETEST_ARCHIVE" ) 2>/dev/null; then fail "archive is writable"; fi
if ( : >> "$AA_SYSROOT_MANIFEST" ) 2>/dev/null; then fail "manifest is writable"; fi
: > "$AA_SYSROOT_CACHE/ok" || fail "cache not writable"
: > build-output || fail "tree not writable"
python3 - <<'PY' || exit 1
import os, sys
# Inherited fds: env -i clears variables, not descriptors. Every fd above 2 must be closed.
for fd in range(3, 256):
    try:
        os.fstat(fd)
    except OSError:
        continue
    print(f'SANDBOX-LEAK: inherited fd {fd} (target {os.readlink(f"/proc/self/fd/{fd}")})')
    sys.exit(1)
PY
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
              extra: tuple[str, ...] = (), backend: str, env_extra: dict[str, str] | None = None,
              path_prefix: str = '', leak_fds: bool = False,
              interrupt=None, drop_env: tuple[str, ...] = ()
              ) -> tuple[int, str, str]:
    scratch = tmp / 'scratch'
    outside = tmp / 'outside-marker'
    outside.write_text('owner-only\n', encoding='utf-8')
    home = tmp / 'home'
    (home / '.config' / 'gh').mkdir(parents=True, exist_ok=True)
    (home / '.config' / 'gh' / 'hosts.yml').write_text(f'canary: {SANDBOX_CANARY}\n', encoding='utf-8')
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
        'PATH': f'{path_prefix}{fakebin}:{os.environ["PATH"]}',
        'HOME': str(home),
        'GH_TOKEN': SANDBOX_CANARY,
        'GITHUB_TOKEN': SANDBOX_CANARY,
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
        'AA_RELAY_TEST_SANDBOX_CMD': probe.replace('@OUTSIDE@', shlex.quote(str(outside)))
                                         .replace('@ORIG_GH@', shlex.quote(str(home / '.config' / 'gh')))
                                         .replace('@ORIG_HOME@', shlex.quote(str(home))).replace('@PORT@', str(port)),
        'GH_LOG': str(gh_log),
        'OUTSIDE_MARKER': str(outside),
        'FAKE_PR_AUTHOR': author,
        'FAKE_PR_HEAD': sha,
    }
    if 'AA_RELAY_IMAGE' in os.environ:
        env['AA_RELAY_IMAGE'] = os.environ['AA_RELAY_IMAGE']
    env.update(env_extra or {})
    for name in drop_env:
        env.pop(name, None)
    args = ['bash', str(RELAY), *extra, sha] + ([pr] if pr else [])
    # Leak a regular-file fd, a directory fd (the owner's HOME, so openat() reaches .config/gh)
    # and a socket fd into the relay; they must not survive into the sandbox.
    held: list[int] = []
    pass_fds: tuple[int, ...] = ()
    if leak_fds:
        spare_a, spare_b = socket.socketpair()
        sources = [os.open(home / '.config' / 'gh' / 'hosts.yml', os.O_RDONLY),
                   os.open(home, os.O_RDONLY | os.O_DIRECTORY), spare_a.fileno()]
        for number, source in zip((40, 41, 42), sources):
            os.dup2(source, number, inheritable=True)
        held += [40, 41, 42, sources[0], sources[1], spare_a.detach(), spare_b.detach()]
        pass_fds = (40, 41, 42)
    try:
        if interrupt is None:
            done = subprocess.run(args, env=env, capture_output=True, text=True, timeout=180, check=False,
                                  pass_fds=pass_fds, input='stdin-must-not-reach-the-sandbox\n')
        else:  # interrupt(pid) runs in a thread while the relay is working
            proc = subprocess.Popen(args, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE, text=True)
            threading.Thread(target=interrupt, args=(proc.pid,), daemon=True).start()
            out, errtxt = proc.communicate(timeout=180)
            done = subprocess.CompletedProcess(args, proc.returncode, out, errtxt)
    finally:
        for number in held:
            try:
                os.close(number)
            except OSError:
                pass
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
        shown = detail.replace(SANDBOX_CANARY, '<canary-redacted>')
        print(f'FAIL: {message}\n{shown}')
        raise SystemExit(1)


FAKE_DOCKER = r"""#!/bin/sh
# Fake docker: `run` hangs, and kill/rm/ps behave as the scenario says.
case "$1" in
    image) exit 0 ;;
    run) exec sleep 300 ;;
    kill | rm) exit 1 ;;
    ps) [ "$FAKE_DOCKER_PS" = fail ] && exit 1; echo deadbeefdeadbeef; exit 0 ;;
esac
exit 0
"""


def usable_docker_image() -> str | None:
    if shutil.which('docker') is None:
        return None
    wanted = [os.environ['AA_RELAY_IMAGE']] if 'AA_RELAY_IMAGE' in os.environ else []
    listed = subprocess.run(['docker', 'images', '--format', '{{.Repository}}:{{.Tag}}'], capture_output=True,
                            text=True, check=False)
    candidates = wanted + [i for i in listed.stdout.split() if '<none>' not in i]
    for image in candidates:
        probe = subprocess.run(['docker', 'run', '--rm', '--network', 'none', image, 'sh', '-c', 'command -v sleep'],
                               capture_output=True, text=True, check=False, timeout=120)
        if probe.returncode == 0:
            return image
    return None


def docker_names(prefix: str) -> str:
    return subprocess.run(['docker', 'ps', '-a', '-q', '--no-trunc', '--filter', f'name={prefix}'],
                          capture_output=True, text=True, check=True).stdout.strip()


def run_container_tests(sha: str) -> None:
    # 6a. Unconfirmed termination (fake docker, deterministic): the client dies on timeout, the
    #     "container" cannot be killed or is still listed -> no status, non-zero exit, tree kept.
    for ps_mode in ('listed', 'fail'):
        with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
            tmp = Path(t)
            fake = tmp / 'fakedocker'
            fake.mkdir()
            (fake / 'docker').write_text(FAKE_DOCKER, encoding='utf-8')
            (fake / 'docker').chmod(0o755)
            rc, err, gh_log = run_relay(
                tmp, sha, probe='true', backend='docker', path_prefix=f'{fake}:',
                env_extra={'AA_RELAY_IMAGE': 'fake:image', 'AA_RELAY_STEP_TIMEOUT': '1', 'AA_RELAY_KILL_AFTER': '1',
                           'FAKE_DOCKER_PS': ps_mode})
            check(rc == 70 and 'unconfirmed' in err, f'unconfirmed termination ({ps_mode}) must exit 70', err)
            check(gh_log == '', f'no status may be posted when termination is unconfirmed ({ps_mode})', gh_log)
            check((tmp / 'scratch' / 'pr-tree').exists(), 'the tree must be left in place when termination is unconfirmed')
            shutil.rmtree(tmp / 'scratch' / 'pr-tree', ignore_errors=True)

    # 6b. Real docker: a container that ignores SIGTERM must be gone after the step timeout, and
    #     the failure status must be posted only after that.
    image = usable_docker_image()
    if image is None:
        print('SKIP (container section): no usable docker daemon + local image with sh/sleep')
        return
    prefix = f'aa-relay-test-{os.getpid()}'
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        try:
            rc, err, gh_log = run_relay(
                tmp, sha, backend='docker', probe='trap "" TERM; while :; do sleep 1; done',
                env_extra={'AA_RELAY_IMAGE': image, 'AA_RELAY_STEP_TIMEOUT': '3', 'AA_RELAY_KILL_AFTER': '2',
                           'AA_RELAY_CONTAINER_PREFIX': prefix})
            check(rc == 1 and 'state=failure' in gh_log, 'a timed-out container step must post failure', err + gh_log)
            check(docker_names(prefix) == '', 'the container survived its client being killed (docker kill/rm missing)',
                  docker_names(prefix))
            check(err.index('sandbox exited step=10-sandbox-probe') < err.index('GH-CALL'),
                  'status posted before the container was confirmed gone', err)
        finally:
            leftovers = docker_names(prefix)
            if leftovers:
                subprocess.run(['docker', 'rm', '-f', *leftovers.split()], capture_output=True, check=False)

    # 6c. SIGTERM to the relay while the container runs: the container is gone and nothing is posted.
    prefix = f'aa-relay-test-sig-{os.getpid()}'

    def terminate_when_running(pid: int) -> None:
        for _ in range(120):
            if docker_names(prefix):
                break
            time.sleep(0.5)
        os.kill(pid, signal.SIGTERM)

    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        try:
            rc, err, gh_log = run_relay(
                tmp, sha, backend='docker', probe='trap "" TERM; while :; do sleep 1; done',
                env_extra={'AA_RELAY_IMAGE': image, 'AA_RELAY_STEP_TIMEOUT': '300',
                           'AA_RELAY_CONTAINER_PREFIX': prefix}, interrupt=terminate_when_running)
            check(rc == 143 and gh_log == '', 'SIGTERM must stop the relay without posting a status', err + gh_log)
            check(docker_names(prefix) == '', 'the container survived SIGTERM of the relay', docker_names(prefix))
        finally:
            leftovers = docker_names(prefix)
            if leftovers:
                subprocess.run(['docker', 'rm', '-f', *leftovers.split()], capture_output=True, check=False)


def main() -> int:
    backend = detect_backend()
    sha = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                         check=True).stdout.strip()
    print(f'sandbox backend under test: {backend}')

    # 1. Passing sandbox: isolation holds, status posted after the sandbox, outside it.
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        rc, err, gh_log = run_relay(tmp, sha, probe=PROBE, backend=backend, leak_fds=True)
        sandbox_log = read_step_log(tmp, '10-sandbox-probe')
        check(rc == 0, 'relay with a passing probe must exit 0', err + sandbox_log)
        check('SANDBOX-OK' in sandbox_log and 'SANDBOX-LEAK' not in sandbox_log,
              'sandbox probe did not pass', sandbox_log)
        check(SANDBOX_CANARY not in sandbox_log, 'canary leaked into the sandbox environment')
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

    # 5. Mount overlap (finding P4): a writable path that equals, contains or sits inside a
    #    read-only input is refused before anything runs; the sysroot stays untouched.
    for label, cache_of in (
            ('cache is the parent of the sysroot', lambda t: t / 'sysroot'),
            ('cache is the sysroot itself', lambda t: t / 'sysroot' / 'rootfs'),
            ('cache lives inside the sysroot', lambda t: t / 'sysroot' / 'rootfs' / 'cache'),
            ('cache is a symlink to the sysroot', lambda t: t / 'rootfs-link')):
        with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
            tmp = Path(t)
            (tmp / 'sysroot' / 'rootfs').mkdir(parents=True)
            (tmp / 'rootfs-link').symlink_to(tmp / 'sysroot' / 'rootfs')
            rc, err, gh_log = run_relay(tmp, sha, probe=PROBE.replace('SANDBOX-OK', 'SANDBOX-RAN'),
                                        backend=backend, env_extra={'AA_SYSROOT_CACHE': str(cache_of(tmp))})
            check(rc == 65 and 'overlaps protected path' in err, f'{label}: must be refused', err)
            check(gh_log == '' and not (tmp / 'scratch' / 'logs' / '10-sandbox-probe.log').exists(),
                  f'{label}: nothing may run or be posted', gh_log)
            check(not (tmp / 'sysroot' / 'rootfs' / 'cache').exists(), f'{label}: host sysroot was modified')
    # Round-2: the trusted scratch (logs, base checkout, empty HOME), the owner's HOME, the
    # repo checkout and its git dirs are protected too. AA_SYSROOT_CACHE/scratch may not equal,
    # contain or (for scratch) sit inside them: exit 65, nothing runs, nothing posted.
    git_dir = subprocess.run(['git', '-C', str(ROOT), 'rev-parse', '--absolute-git-dir'], capture_output=True,
                             text=True, check=True).stdout.strip()
    for label, cache_of in (
            ('cache is the owner HOME', lambda t: t / 'home'),
            ('cache is the parent of the owner HOME', lambda t: t),
            ('cache is the repo root', lambda t: ROOT),
            ('cache is the repo git dir', lambda t: Path(git_dir)),
            ('cache is inside the repo git dir', lambda t: Path(git_dir) / 'aa-relay-test-cache'),
            ('cache is the trusted logs dir', lambda t: t / 'scratch' / 'logs'),
            ('cache is inside the trusted logs dir', lambda t: t / 'scratch' / 'logs' / 'c'),
            ('cache is the base checkout', lambda t: t / 'scratch' / 'base'),
            ('cache is the empty HOME', lambda t: t / 'scratch' / 'empty-home'),
            ('cache is the scratch root', lambda t: t / 'scratch')):
        with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
            tmp = Path(t)
            rc, err, gh_log = run_relay(tmp, sha, probe=PROBE.replace('SANDBOX-OK', 'SANDBOX-RAN'),
                                        backend=backend, env_extra={'AA_SYSROOT_CACHE': str(cache_of(tmp))})
            check(rc == 65 and ('overlaps protected path' in err or 'equals or contains protected path' in err),
                  f'{label}: must be refused', err)
            check(gh_log == '' and not (tmp / 'scratch' / 'logs' / '10-sandbox-probe.log').exists(),
                  f'{label}: nothing may run or be posted', gh_log)
        check(not (Path(git_dir) / 'aa-relay-test-cache').exists(), f'{label}: touched the repo git dir')
    # A cache that merely sits inside the owner's HOME (not HOME itself) stays allowed.
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        rc, err, gh_log = run_relay(tmp, sha, probe='true', backend=backend,
                                    env_extra={'AA_SYSROOT_CACHE': str(tmp / 'home' / 'cache' / 'aa')})
        check(rc == 0, 'a dedicated cache directory inside HOME must still work', err)
    # Default scratch (no AA_RELAY_SCRATCH): a fresh unpredictable 0700 directory under $TMPDIR
    # (else /var/tmp), never under $XDG_RUNTIME_DIR (RAM tmpfs) and not /tmp/<name>-<sha>.
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        runtime = tmp / 'xdg'
        runtime.mkdir(mode=0o755)
        tmpdir = tmp / 'tmpdir'
        tmpdir.mkdir(mode=0o755)
        predictable = tmpdir / f'arm64-cross-relay-{sha}'
        rc, err, gh_log = run_relay(tmp, sha, probe='true', backend=backend,
                                    env_extra={'XDG_RUNTIME_DIR': str(runtime), 'TMPDIR': str(tmpdir),
                                               'AA_RELAY_SCRATCH': ''})
        made = sorted(tmpdir.glob('arm64-cross-relay.*'))
        check(rc == 0 and len(made) == 1, 'default scratch must be one mktemp dir under TMPDIR', err)
        check(not list(runtime.iterdir()), 'the default scratch must not live under XDG_RUNTIME_DIR')
        check(stat.S_IMODE(made[0].stat().st_mode) == 0o700, 'scratch must be mode 0700', oct(made[0].stat().st_mode))
        check(not predictable.exists(), 'relay used a predictable scratch path')
        # Success removes everything except the logs and says where the scratch is.
        check(sorted(p.name for p in made[0].iterdir()) == ['.aa-relay-dedicated', 'logs'],
              'success must leave only the logs (and the marker)', str(sorted(p.name for p in made[0].iterdir())))
        check(f'scratch kept at {made[0]}' in err, 'the scratch path must be printed at exit', err)
    # Without TMPDIR the default is /var/tmp, never XDG_RUNTIME_DIR.
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        runtime = tmp / 'xdg'
        runtime.mkdir(mode=0o755)
        before = set(Path('/var/tmp').glob('arm64-cross-relay.*'))
        rc, err, gh_log = run_relay(tmp, sha, probe='true', backend=backend, drop_env=('TMPDIR',),
                                    env_extra={'XDG_RUNTIME_DIR': str(runtime), 'AA_RELAY_SCRATCH': ''})
        made = set(Path('/var/tmp').glob('arm64-cross-relay.*')) - before
        try:
            check(rc == 0 and len(made) == 1 and not list(runtime.iterdir()),
                  'without TMPDIR the default scratch must be /var/tmp, not XDG_RUNTIME_DIR', err)
        finally:
            for d in made:
                shutil.rmtree(d, ignore_errors=True)
    # Early exits must not leak the default scratch: missing commit, non-owner author, PR head mismatch.
    for label, kwargs, want_rc in (
            ('missing commit', {'sha_override': 'a' * 40}, 66),
            ('non-owner author', {'pr': '7', 'author': 'someone-else'}, 65),
            ('PR head mismatch', {'pr': '7', 'head_override': 'b' * 40}, 65)):
        with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
            tmp = Path(t)
            tmpdir = tmp / 'tmpdir'
            tmpdir.mkdir(mode=0o755)
            rc, err, gh_log = run_relay(tmp, kwargs.get('sha_override', sha), probe='true', backend=backend,
                                        pr=kwargs.get('pr'), author=kwargs.get('author', 'zucch1'),
                                        env_extra={'TMPDIR': str(tmpdir), 'AA_RELAY_SCRATCH': '',
                                                   **({'FAKE_PR_HEAD': kwargs['head_override']}
                                                      if 'head_override' in kwargs else {})})
            check(rc == want_rc, f'{label}: expected exit {want_rc}', err)
            check(not list(tmpdir.iterdir()), f'{label}: leaked a scratch directory', str(list(tmpdir.iterdir())))
            check('removed (nothing to keep)' in err, f'{label}: the removal must be reported', err)
    # Round 3: the repo root is fully protected (not just "may not contain it"): a cache that
    # IS or sits inside the checkout (build/, tools/ci/) could let the sandbox rewrite the host
    # build.ninja or the running relay script. Nothing is created in the checkout.
    for label, cache_of in (('cache is the repo build dir', lambda t: ROOT / 'build'),
                            ('cache is the repo tools/ci dir', lambda t: ROOT / 'tools' / 'ci'),
                            ('cache is a new dir inside the repo', lambda t: ROOT / 'aa-relay-test-cache')):
        existed = cache_of(None).exists()
        with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
            tmp = Path(t)
            rc, err, gh_log = run_relay(tmp, sha, probe=PROBE.replace('SANDBOX-OK', 'SANDBOX-RAN'), backend=backend,
                                        env_extra={'AA_SYSROOT_CACHE': str(cache_of(tmp))})
            check(rc == 65 and 'overlaps protected path' in err, f'{label}: must be refused', err)
            check(gh_log == '' and not (tmp / 'scratch').exists(), f'{label}: nothing may run, be posted or be created', gh_log)
        check(cache_of(None).exists() == existed, f'{label}: the checkout was modified')
        check(not (ROOT / 'aa-relay-test-cache').exists(), f'{label}: created a directory in the checkout')
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:  # scratch inside the checkout
        tmp = Path(t)
        rc, err, gh_log = run_relay(tmp, sha, probe='true', backend=backend,
                                    env_extra={'AA_RELAY_SCRATCH': str(ROOT / 'aa-relay-test-scratch')})
        check(rc == 65 and 'overlaps protected path' in err, 'scratch inside the repo checkout must be refused', err)
        check(not (ROOT / 'aa-relay-test-scratch').exists(), 'created a scratch inside the checkout')
    # Round 3: AA_SYSROOT_CACHE / AA_RELAY_SCRATCH must be nonexistent, empty, or marked by the
    # relay itself; a non-empty unmarked directory (~/.ssh, ~/.config, a project) is refused (65).
    for which in ('AA_SYSROOT_CACHE', 'AA_RELAY_SCRATCH'):
        with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
            tmp = Path(t)
            for victim_name in ('.ssh', '.config'):
                victim = tmp / 'home' / victim_name
                victim.mkdir(parents=True, exist_ok=True)
                victim.chmod(0o700)
                (victim / 'keep-me').write_text('owner data\n', encoding='utf-8')
                rc, err, gh_log = run_relay(tmp, sha, probe='touch "$AA_SYSROOT_CACHE/planted"; true', backend=backend,
                                            env_extra={which: str(victim)})
                check(rc == 65 and 'non-empty directory the relay did not create' in err,
                      f'{which}={victim_name}: a non-empty unmarked directory must be refused', err)
                check(gh_log == '' and (victim / 'keep-me').is_file()
                      and not (victim / 'planted').exists() and not (victim / '.aa-relay-dedicated').exists(),
                      f'{which}={victim_name}: the directory must be untouched and nothing posted', gh_log)
            # A regular file in place of the directory is refused as well.
            afile = tmp / 'afile'
            afile.write_text('x', encoding='utf-8')
            rc, err, gh_log = run_relay(tmp, sha, probe='true', backend=backend, env_extra={which: str(afile)})
            check(rc == 65 and 'not a directory' in err, f'{which}: a file must be refused', err)
        # Nonexistent: created on first use with the marker. Empty: adopted, marker added.
        # Marked: reused although non-empty.
        for state in ('nonexistent', 'empty', 'marked'):
            with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
                tmp = Path(t)
                target = tmp / 'dedicated'
                if state != 'nonexistent':
                    target.mkdir(mode=0o700)
                if state == 'marked':
                    (target / '.aa-relay-dedicated').write_text('', encoding='utf-8')
                    (target / 'earlier-run-leftover').write_text('ok\n', encoding='utf-8')
                rc, err, gh_log = run_relay(tmp, sha, probe=PROBE if which == 'AA_SYSROOT_CACHE' else 'true',
                                            backend=backend, env_extra={which: str(target)})
                check(rc == 0, f'{which} ({state}) must be accepted', err)
                check((target / '.aa-relay-dedicated').is_file(), f'{which} ({state}): marker missing after use')
                if which == 'AA_SYSROOT_CACHE':
                    check((target / 'ok').exists(), f'{which} ({state}): the sandbox could not write the cache')
                if state == 'marked':
                    check((target / 'earlier-run-leftover').exists(), f'{which}: a marked directory was cleaned out')
    # An existing explicit scratch must be owned by the current user and not group/world-writable.
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        shared = tmp / 'scratch'
        shared.mkdir()
        (shared / '.aa-relay-dedicated').write_text('', encoding='utf-8')  # marked, so only mode can refuse it
        for mode in (0o770, 0o707, 0o777):
            shared.chmod(mode)
            rc, err, gh_log = run_relay(tmp, sha, probe='true', backend=backend)
            check(rc == 65 and 'group- or world-writable' in err, f'scratch mode {oct(mode)} must be refused', err)
            check(gh_log == '', 'nothing may be posted for a writable scratch', gh_log)
        shared.chmod(0o755)
        rc, err, gh_log = run_relay(tmp, sha, probe='true', backend=backend)
        check(rc == 0, 'a 0755 marked scratch of the current user must be accepted', err)
    if os.getuid() != 0:
        foreign = next((Path(d) for d in ('/srv', '/mnt', '/media', '/opt') if Path(d).is_dir()
                        and Path(d).stat().st_uid != os.getuid() and not Path(d).stat().st_mode & 0o022), None)
        if foreign is not None:
            with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
                rc, err, gh_log = run_relay(Path(t), sha, probe='true', backend=backend,
                                            env_extra={'AA_RELAY_SCRATCH': str(foreign)})
                check(rc == 65 and 'not owned by the current user' in err,
                      f'a scratch owned by another user ({foreign}) must be refused', err)
        else:
            print('SKIP (ownership case): no root-owned, non-writable /srv, /mnt, /media or /opt here')
    # Ordering: writable mounts first, caller's read-only inputs last (a later mount wins).
    with tempfile.TemporaryDirectory(prefix='relay-sb-test-') as t:
        tmp = Path(t)
        for name in ('rw1', 'rw2', 'ro1', 'ro2'):
            (tmp / name).mkdir()
        script = (f'. {shlex.quote(str(LIB))}; RELAY_SB_RW=({tmp}/rw1 {tmp}/rw2); RELAY_SB_RO=({tmp}/ro1 {tmp}/ro2); '
                  f'RELAY_SB_ENV=(); relay_sandbox_build_argv bwrap {tmp}/rw1 true || exit $?; '
                  'printf "%s\\n" "${RELAY_SB_ARGV[@]}"')
        done = subprocess.run(['bash', '-c', script], capture_output=True, text=True, check=False)
        check(done.returncode == 0, 'argv construction failed', done.stderr)
        argv = done.stdout.splitlines()
        rw_at = [i for i, a in enumerate(argv) if a == '--bind' and argv[i + 1].startswith(str(tmp))]
        ro_at = [i for i, a in enumerate(argv) if a == '--ro-bind' and argv[i + 1].startswith(str(tmp))]
        check(len(rw_at) == 2 and len(ro_at) == 2 and max(rw_at) < min(ro_at),
              'read-only mounts must come after every writable mount', '\n'.join(argv))

    # 6. Container termination (finding P5).
    run_container_tests(sha)

    print('relay sandbox test: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
