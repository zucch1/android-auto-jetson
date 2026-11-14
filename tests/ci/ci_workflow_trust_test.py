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
        print(f'FAIL: {message}')
        os.write(2, (detail + '\n').encode('utf-8', 'replace'))
        raise SystemExit(1)


def main() -> int:
    try:
        import yaml
    except ImportError:
        print('SKIP: PyYAML is not installed')
        return 77
    doc = yaml.safe_load(WORKFLOW.read_text(encoding='utf-8'))
    jobs = doc['jobs']

    for job_id in ('host-build-test', 'base-validators', 'arm64-cross-fixture'):
        job = jobs.get(job_id)
        check(job is not None, f'ci.yml is missing required/gating job {job_id!r}')
        check('needs' not in job,
              f'job {job_id!r} must not depend on a job that runs PR code or could skip it')
        check('if' not in job,
              f'job {job_id!r} must not have a job-level `if:` (a skipped required job counts as passing)')
    trusted = jobs['base-validators']
    triggers = doc.get('on', doc.get(True))
    check({'pull_request', 'push', 'workflow_dispatch'} <= set(triggers), 'unexpected trigger set', str(triggers))
    pr_types = set(triggers['pull_request'].get('types', []))
    check(pr_types == {'opened', 'synchronize', 'reopened', 'edited'},
          'pull_request types must include `edited` (opened/synchronize/reopened/edited) so retargeting '
          'reruns CI instead of reusing stale green required checks, without job-level `if:` skips')
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
    gate_if = str(gate.get('if', '')).strip()
    check(gate_if in ('!cancelled()', '${{ !cancelled() }}'),
          'ci/provenance-lint `if:` must be exactly `!cancelled()` so it always runs and never skips on edited/retarget')
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

    # Merge-parent verification in ci/provenance-lint (after trusted gate and checkout, before PR scripts).
    checkout = gate['steps'][1]
    check(checkout.get('uses', '').startswith('actions/checkout@')
          and (checkout.get('with') or {}).get('ref') is None,
          'step 2 of ci/provenance-lint must check out the PR merge ref without a ref override')
    fetch_depth = (checkout.get('with') or {}).get('fetch-depth')
    check(isinstance(fetch_depth, int) and not isinstance(fetch_depth, bool)
          and (fetch_depth == 0 or fetch_depth >= 2),
          'ci/provenance-lint checkout must set fetch-depth >= 2 (or 0) so HEAD^1 and HEAD^2 exist')

    verify = gate['steps'][2]
    verify_if = str(verify.get('if', '')).strip()
    check(verify_if in ("github.event_name == 'pull_request'", "${{ github.event_name == 'pull_request' }}"),
          'merge-parent verification step must run only on pull_request events')
    verify_env = verify.get('env') or {}
    base_keys = [k for k, v in verify_env.items() if str(v).strip() == '${{ github.event.pull_request.base.sha }}']
    head_keys = [k for k, v in verify_env.items() if str(v).strip() == '${{ github.event.pull_request.head.sha }}']
    check(len(base_keys) == 1 and len(head_keys) == 1,
          'merge-parent verification step must pass github.event.pull_request.base.sha and head.sha through env:')
    base_env_key, head_env_key = base_keys[0], head_keys[0]
    verify_script = verify.get('run', '')
    check(bool(verify_script) and '${{' not in verify_script,
          'merge-parent verification script must not interpolate ${{ }} directly into the shell body')
    for needle in ('HEAD^1', 'HEAD^2'):
        check(needle in verify_script, f'merge-parent verification script is missing {needle!r}', verify_script)

    with tempfile.TemporaryDirectory(prefix='ci-merge-parents-test-') as mt:
        mp = Path(mt)
        remote = mp / 'remote'
        remote.mkdir()
        rgit = ['git', '-C', str(remote), '-c', 'user.name=t', '-c', 'user.email=t@example.invalid']
        subprocess.run([*rgit, 'init', '-q', '-b', 'main'], check=True)
        (remote / 'f.txt').write_text('base1\n', encoding='utf-8')
        subprocess.run([*rgit, 'add', 'f.txt'], check=True)
        subprocess.run([*rgit, 'commit', '-q', '-m', 'base1'], check=True)
        base1 = subprocess.run([*rgit, 'rev-parse', 'HEAD'], check=True, capture_output=True, text=True).stdout.strip()

        subprocess.run([*rgit, 'checkout', '-q', '-b', 'pr-head'], check=True)
        (remote / 'pr.txt').write_text('head1\n', encoding='utf-8')
        subprocess.run([*rgit, 'add', 'pr.txt'], check=True)
        subprocess.run([*rgit, 'commit', '-q', '-m', 'head1'], check=True)
        head1 = subprocess.run([*rgit, 'rev-parse', 'HEAD'], check=True, capture_output=True, text=True).stdout.strip()

        subprocess.run([*rgit, 'checkout', '-q', 'main'], check=True)
        (remote / 'f.txt').write_text('base2\n', encoding='utf-8')
        subprocess.run([*rgit, 'add', 'f.txt'], check=True)
        subprocess.run([*rgit, 'commit', '-q', '-m', 'base2'], check=True)
        base2 = subprocess.run([*rgit, 'rev-parse', 'HEAD'], check=True, capture_output=True, text=True).stdout.strip()

        subprocess.run([*rgit, 'checkout', '-q', '--detach', base1], check=True)
        subprocess.run([*rgit, 'merge', '-q', '--no-ff', '-m', 'pr merge', head1], check=True)
        merge_ok = subprocess.run([*rgit, 'rev-parse', 'HEAD'], check=True, capture_output=True, text=True).stdout.strip()
        subprocess.run([*rgit, 'update-ref', 'refs/pull/1/merge', merge_ok], check=True)

        subprocess.run([*rgit, 'checkout', '-q', '--detach', base1], check=True)
        subprocess.run([*rgit, 'merge', '-q', '--no-ff', '-m', 'octopus', head1, base2], check=True)
        merge_octopus = subprocess.run([*rgit, 'rev-parse', 'HEAD'], check=True, capture_output=True, text=True).stdout.strip()
        subprocess.run([*rgit, 'update-ref', 'refs/pull/2/merge', merge_octopus], check=True)

        def make_checkout(name: str, ref: str, depth: int) -> Path:
            cdir = mp / name
            cdir.mkdir()
            cgit = ['git', '-C', str(cdir)]
            subprocess.run([*cgit, 'init', '-q'], check=True)
            subprocess.run([*cgit, 'remote', 'add', 'origin', f'file://{remote}'], check=True)
            subprocess.run([*cgit, 'fetch', '-q', '--no-tags', f'--depth={depth}', 'origin', f'+{ref}:{ref}'], check=True)
            subprocess.run([*cgit, 'checkout', '-q', '--detach', ref], check=True)
            return cdir

        def run_verify(cdir: Path, exp_base: str, exp_head: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                ['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', verify_script],
                env={'PATH': '/usr/bin:/bin', base_env_key: exp_base, head_env_key: exp_head},
                cwd=cdir, capture_output=True, text=True, check=False,
            )

        c_ok = make_checkout('ok-d2', 'refs/pull/1/merge', 2)
        res = run_verify(c_ok, base1, head1)
        check(res.returncode == 0, 'matching depth=2 PR merge checkout must pass', res.stdout + res.stderr)

        # Stale base after retarget (expected base2, actual base1)
        res = run_verify(c_ok, base2, head1)
        out = res.stdout + res.stderr
        check(res.returncode != 0 and base2 in out and base1 in out,
              'stale base SHA must fail and print both expected and actual SHAs', out)

        # Stale head (expected base2, actual head1)
        res = run_verify(c_ok, base1, base2)
        out = res.stdout + res.stderr
        check(res.returncode != 0 and base2 in out and head1 in out,
              'stale head SHA must fail and print both expected and actual SHAs', out)

        # Swapped parents
        res = run_verify(c_ok, head1, base1)
        out = res.stdout + res.stderr
        check(res.returncode != 0 and head1 in out and base1 in out,
              'swapped parent SHAs must fail and print expected/actual SHAs', out)

        # Empty expected SHA
        res = run_verify(c_ok, '', head1)
        check(res.returncode != 0, 'empty expected SHA must fail', res.stdout + res.stderr)

        # Non-merge commit (only 1 parent)
        c_nonmerge = make_checkout('nonmerge-d2', 'refs/heads/pr-head', 2)
        res = run_verify(c_nonmerge, base1, head1)
        out = res.stdout + res.stderr
        check(res.returncode != 0 and base1 in out and head1 in out,
              'non-merge commit must fail and print expected/actual info', out)

        # 3-parent octopus merge
        c_oct = make_checkout('octopus-d2', 'refs/pull/2/merge', 2)
        res = run_verify(c_oct, base1, head1)
        check(res.returncode != 0, '3-parent merge commit must fail', res.stdout + res.stderr)

        # Shallow depth=1 graft (parents unavailable)
        c_d1 = make_checkout('shallow-d1', 'refs/pull/1/merge', 1)
        res = run_verify(c_d1, base1, head1)
        check(res.returncode != 0, 'depth=1 shallow merge checkout without parents must fail', res.stdout + res.stderr)

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
