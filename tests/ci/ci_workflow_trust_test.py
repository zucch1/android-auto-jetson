# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression test for the trusted base-revision validators in .github/workflows/ci.yml (review finding P1).

Threat: PR-controlled steps that run BEFORE the "trusted" validator steps of the same job can write
BASH_ENV (or PATH) through $GITHUB_ENV / $GITHUB_PATH, e.g. defining a `python3()` shell function that
returns 0, so the later steps pass without running anything. Fix under test: the validators live in
their own job (fresh runner, nothing of the PR executed first), validator code comes from the base
revision, the PR tree is only a data argument, interpreters are absolute and the env is scrubbed.
The required context `ci/provenance-lint` depends on that job and cannot pass without it.

Checks the structure of the workflow, then runs the job's real `run` script under a poisoned BASH_ENV
with nonexistent validator paths and proves it still FAILS (it must not be satisfiable by shell functions).
Skips (77) when PyYAML is unavailable.
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github' / 'workflows' / 'ci.yml'


def check(condition: bool, message: str, detail: str = '') -> None:
    # `detail` is only ever subprocess output or a script body, never workflow step data (those
    # dicts carry `persist-credentials`, which CodeQL treats as a secret source when logged).
    if not condition:
        print(f'FAIL: {message}\n{detail}')
        raise SystemExit(1)


def main() -> int:
    try:
        import yaml
    except ImportError:
        print('SKIP: PyYAML is not installed')
        return 77
    doc = yaml.safe_load(WORKFLOW.read_text(encoding='utf-8'))
    jobs = doc['jobs']

    trusted = jobs.get('base-validators')
    check(trusted is not None, 'ci.yml has no separate `base-validators` job for the trusted validators')
    check('needs' not in trusted, 'the trusted job must not depend on a job that runs PR code first')
    check('if' not in trusted, 'the trusted job must run on EVERY event (pull_request, push, workflow_dispatch): '
          'a skipped base-validators job is the workflow_dispatch bypass')
    triggers = doc.get('on', doc.get(True))
    check({'pull_request', 'push', 'workflow_dispatch'} <= set(triggers), 'unexpected trigger set', str(triggers))
    pr_types = set(triggers['pull_request'].get('types', []))
    check(pr_types == {'opened', 'synchronize', 'reopened'},
          'pull_request types must be exactly opened/synchronize/reopened: `edited` restarts full CI on every '
          'title/body edit (retarget: close+reopen or push), and no job-level `if:` skip may replace it')
    steps = trusted['steps']
    runs = [s for s in steps if 'run' in s]
    uses = [s for s in steps if 'uses' in s]
    check(len(runs) == 1 and steps[-1] is runs[0], 'exactly one run step, and it comes last (after the checkouts)')
    check(all(s['uses'].startswith('actions/checkout@') for s in uses), 'only the pinned checkout action may precede it')
    refs = {(s.get('with') or {}).get('path'): (s.get('with') or {}).get('ref') for s in uses}
    check(refs.get('base') == 'main' and refs.get('pr') is None,
          'validator code must come from the literal ref `main` (not the PR base ref, which can be an '
          'author-pushable aa/** branch, and not an expression that could be empty: actions/checkout then '
          'silently falls back to the event ref); the tree under test is checked out separately as data')
    check('pull_request' not in json.dumps(uses) and 'head' not in json.dumps(refs),
          'validator checkout must not reference PR-controlled refs')
    script = runs[0]['run']
    for needle in ('/usr/bin/env -i', '/usr/bin/python3', '-B -s -E', 'tools/deps/check_manifest.py',
                   'tools/provenance/check.py', 'tools/tls/scan.py', 'symbolic-ref --short HEAD',
                   'refs/remotes/origin/main'):
        check(needle in script, f'trusted run step is missing {needle!r}', script)
    for forbidden in ('GITHUB_ENV', 'GITHUB_PATH', 'source ', '. "$pr', 'tools/provenance/check.sh', 'bash "$pr'):
        check(forbidden not in script, f'trusted run step must not use {forbidden!r}', script)

    gate = jobs['provenance-lint']
    check(gate.get('needs') == 'base-validators' or 'base-validators' in gate.get('needs', []),
          'ci/provenance-lint must depend on the trusted job')
    check('!cancelled()' in str(gate.get('if', '')),
          'ci/provenance-lint must always run and decide explicitly (a skipped dependency would count as passing)')
    first = gate['steps'][0]
    check('needs.base-validators.result' in str(first.get('env', {})) and 'run' in first,
          'the first step of ci/provenance-lint must check the trusted job result before any PR code runs')
    check(jobs['provenance-lint']['name'] == 'ci/provenance-lint', 'required context name changed')

    # Event/result matrix for the gate step (the real script): ONLY `success` passes, for every
    # event type. `workflow_dispatch:skipped` and `push:skipped` used to pass, which let
    # `gh workflow run ci.yml --ref <pr-branch>` produce a green ci/provenance-lint without the
    # base validators (nothing in the diff).
    gate_script = first['run']
    for event in ('pull_request', 'push', 'workflow_dispatch', 'schedule'):
        for result in ('success', 'skipped', 'failure', 'cancelled', ''):
            env = {'PATH': '/usr/bin:/bin', 'EVENT': event, 'BASE_RESULT': result}
            done = subprocess.run(['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', gate_script],
                                  env=env, capture_output=True, text=True, check=False)
            check((done.returncode == 0) == (result == 'success'),
                  f'gate: event={event!r} result={result!r} returned {done.returncode}',
                  done.stdout + done.stderr)
    # The dispatch-on-a-non-main-branch case in particular: the validator job is skipped there
    # only if someone put an `if:` back, and the gate must still refuse.
    done = subprocess.run(['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', gate_script],
                          env={'PATH': '/usr/bin:/bin', 'EVENT': 'workflow_dispatch', 'BASE_RESULT': 'skipped',
                               'GITHUB_REF': 'refs/heads/aa/some-pr-branch'},
                          capture_output=True, text=True, check=False)
    check(done.returncode != 0, 'workflow_dispatch on a PR branch with base-validators skipped must fail the gate')

    # Executable check: poison the shell the way a PR step could and run the real script.
    with tempfile.TemporaryDirectory(prefix='ci-trust-test-') as t:
        tmp = Path(t)
        ws = tmp / 'ws'
        (ws / 'base').mkdir(parents=True)  # base checkout without the validators: they must be reported missing
        (ws / 'pr').mkdir()
        poison = tmp / 'poison.sh'
        poison.write_text('python3() { return 0; }\nenv() { return 0; }\ncd() { return 0; }\n'
                          'python() { return 0; }\ncheck_manifest() { return 0; }\n', encoding='utf-8')
        env = {'PATH': '/usr/bin:/bin', 'BASH_ENV': str(poison), 'GITHUB_WORKSPACE': str(ws), 'RUNNER_TEMP': str(tmp)}
        done = subprocess.run(['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', script], env=env,
                              capture_output=True, text=True, check=False, cwd=tmp)
        check(done.returncode != 0, 'poisoned BASH_ENV made the trusted step pass with nonexistent validators',
              done.stdout + done.stderr)
        # And with real validators the same poisoned shell still reaches the real interpreter.
        base = tmp / 'ws2' / 'base'
        pr = tmp / 'ws2' / 'pr'
        for d in (base, pr):
            d.mkdir(parents=True)
            subprocess.run(f'git -C {ROOT} archive HEAD | tar -x -C {d}', shell=True, check=True)
        # The base dir must look like actions/checkout of `main`: on branch main at origin/main.
        git = ['git', '-C', str(base), '-c', 'user.name=t', '-c', 'user.email=t@example.invalid']
        subprocess.run([*git, 'init', '-q', '-b', 'main'], check=True)
        subprocess.run([*git, 'add', '-A'], check=True, capture_output=True)
        subprocess.run([*git, 'commit', '-q', '-m', 'base'], check=True, capture_output=True)
        subprocess.run([*git, 'update-ref', 'refs/remotes/origin/main', 'HEAD'], check=True)
        env['GITHUB_WORKSPACE'] = str(tmp / 'ws2')
        done = subprocess.run(['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', script], env=env,
                              capture_output=True, text=True, check=False, cwd=tmp)
        check(done.returncode == 0 and 'provenance-check: PASS' in done.stdout and 'tls-key-scan: PASS' in done.stdout,
              'the validators did not really run under a poisoned shell', done.stdout + done.stderr)
        # A validator checkout that is not main (the silent fallback to the event ref) must fail,
        # however good the validators are.
        env['GITHUB_WORKSPACE'] = str(tmp / 'ws2')
        subprocess.run([*git, 'switch', '-q', '-c', 'event-ref'], check=True)
        done = subprocess.run(['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', script], env=env,
                              capture_output=True, text=True, check=False, cwd=tmp)
        check(done.returncode != 0 and 'not on branch main' in done.stdout,
              'a validator checkout on another ref must fail the trusted job', done.stdout + done.stderr)
        subprocess.run([*git, 'switch', '-q', 'main'], check=True)
    print('ci workflow trust test: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
