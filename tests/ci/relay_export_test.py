# SPDX-License-Identifier: GPL-3.0-or-later
"""Regression test for tools/ci/relay-export-tree.py (review finding P2: `git archive` filters).

The relay exports the UNTRUSTED PR head. `git archive` applies the PR's own .gitattributes, so a
PR can select a filter driver (here configured in the repository's LOCAL config, standing in for
any installed smudge/process filter) or `export-subst`, and the owner's machine runs it with the
owner's HOME and SSH agent. The exporter must write raw blobs only:

* a smudge/clean/process filter selected by .gitattributes never runs;
* `$Format:...$` (export-subst) is not expanded;
* a symlink pointing outside the export root stays a symlink and is never written through, even
  when a hand-crafted tree has a symlink and a directory with the same name;
* gitlinks are skipped, `.git` path components and `..` are rejected, modes are preserved.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPORT = ROOT / 'tools' / 'ci' / 'relay-export-tree.py'
GIT_ENV = {'PATH': '/usr/bin:/bin', 'HOME': '/nonexistent', 'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': '/dev/null',
           'GIT_AUTHOR_NAME': 't', 'GIT_AUTHOR_EMAIL': 't@example.invalid', 'GIT_COMMITTER_NAME': 't',
           'GIT_COMMITTER_EMAIL': 't@example.invalid'}


def check(condition: bool, message: str, detail: str = '') -> None:
    if not condition:
        print(f'FAIL: {message}\n{detail}')
        raise SystemExit(1)


def git(repo: Path, *args: str, stdin: bytes | None = None) -> str:
    done = subprocess.run(['git', '-C', str(repo), *args], env=GIT_ENV, input=stdin, capture_output=True, check=True)
    return done.stdout.decode().strip()


def export(repo: Path, sha: str, dest: Path) -> subprocess.CompletedProcess:
    dest.mkdir()
    return subprocess.run([sys.executable, '-I', '-B', str(EXPORT), str(repo), sha, str(dest)], env=GIT_ENV,
                          capture_output=True, text=True, check=False)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix='relay-export-test-') as t:
        tmp = Path(t)
        repo = tmp / 'repo'
        repo.mkdir()
        git(repo, 'init', '-q')
        marker = tmp / 'FILTER-RAN'
        # The "installed" filter, in the repo's own config. If it runs it drops a marker file.
        for key in ('smudge', 'clean'):
            git(repo, 'config', f'filter.evil.{key}', f'sh -c \'touch {marker}; cat\'')
        git(repo, 'config', 'filter.evil.required', 'false')
        (repo / '.gitattributes').write_text('* filter=evil\nsubst.txt export-subst\n', encoding='utf-8')
        (repo / 'subst.txt').write_text('commit=$Format:%H$\n', encoding='utf-8')
        (repo / 'run.sh').write_text('#!/bin/sh\n', encoding='utf-8')
        (repo / 'run.sh').chmod(0o755)
        (repo / 'dir').mkdir()
        (repo / 'dir' / 'a.txt').write_text('a\n', encoding='utf-8')
        (repo / 'escape').symlink_to('/etc')
        (repo / 'rel').symlink_to('../../outside')
        git(repo, 'add', '--force', '-A')
        marker.unlink(missing_ok=True)  # `git add` itself may run the clean filter; only the export matters
        git(repo, 'commit', '-q', '-m', 'hostile tree')
        marker.unlink(missing_ok=True)
        sha = git(repo, 'rev-parse', 'HEAD')

        # Prove the repository really is hostile: the old method runs the filter and expands export-subst.
        old = tmp / 'old'
        old.mkdir()
        subprocess.run(f'git -C {repo} archive --format=tar {sha} | tar -x -C {old}', shell=True, env=GIT_ENV, check=True)
        check(marker.exists(), 'test setup: `git archive` was expected to run the filter (the vulnerability)')
        check(sha in (old / 'subst.txt').read_text(encoding='utf-8'), 'test setup: git archive expands export-subst')
        marker.unlink()

        dest = tmp / 'export'
        done = export(repo, sha, dest)
        check(done.returncode == 0, 'export failed', done.stderr)
        check(not marker.exists(), 'a filter selected by .gitattributes ran during the export')
        check((dest / 'subst.txt').read_text(encoding='utf-8') == 'commit=$Format:%H$\n', 'export-subst was expanded')
        check(os.access(dest / 'run.sh', os.X_OK) and not os.access(dest / 'dir' / 'a.txt', os.X_OK), 'modes not preserved')
        check((dest / 'escape').is_symlink() and os.readlink(dest / 'escape') == '/etc', 'symlink must stay a symlink')
        check(os.readlink(dest / 'rel') == '../../outside', 'relative symlink altered')
        check(not (dest / '.git').exists(), '.git must not exist in the export')

        # A symlink and a directory of the same name (hand-crafted tree): nothing may be written through the link.
        outside = tmp / 'outside'
        outside.mkdir()
        blob = git(repo, 'hash-object', '-w', '--stdin', stdin=b'pwned\n')
        link = git(repo, 'hash-object', '-w', '--stdin', stdin=str(outside).encode())
        subtree = git(repo, 'mktree', stdin=f'100644 blob {blob}\tpwned\n'.encode())
        crafted = git(repo, 'hash-object', '-t', 'tree', '-w', '--literally', '--stdin', stdin=(
            f'120000 link\0'.encode() + bytes.fromhex(link) + b'40000 link\0' + bytes.fromhex(subtree)))
        commit = git(repo, 'commit-tree', crafted, '-m', 'crafted')
        done = export(repo, commit, tmp / 'export2')
        check(done.returncode != 0, 'a tree with a symlink and a same-name directory must be rejected', done.stderr)
        check(not (outside / 'pwned').exists(), 'the export wrote through a symlink and escaped the root')

        # Unsafe paths are rejected (hand-crafted trees; git itself refuses to stage them).
        for n, bad in enumerate(('.git', '.GIT', '..')):
            tree = git(repo, 'hash-object', '-t', 'tree', '-w', '--literally', '--stdin',
                       stdin=f'40000 {bad}\0'.encode() + bytes.fromhex(subtree))
            done = export(repo, git(repo, 'commit-tree', tree, '-m', 'bad'), tmp / f'export-bad{n}')
            check(done.returncode != 0 and 'unsafe path' in done.stderr, f'path component {bad!r} must be rejected', done.stderr)

        # Gitlinks are skipped, not materialised.
        gitlink = git(repo, 'hash-object', '-t', 'tree', '-w', '--literally', '--stdin',
                      stdin=b'160000 sub\0' + bytes.fromhex(sha) + b'100644 ok\0' + bytes.fromhex(blob))
        done = export(repo, git(repo, 'commit-tree', gitlink, '-m', 'gitlink'), tmp / 'export-gitlink')
        check(done.returncode == 0 and (tmp / 'export-gitlink' / 'ok').exists() and not (tmp / 'export-gitlink' / 'sub').exists(),
              'gitlink handling', done.stderr)

        # refs/replace/* in the owner's repo must not rewrite the PR's objects (GIT_NO_REPLACE_OBJECTS=1).
        orig_blob = git(repo, 'rev-parse', f'{sha}:dir/a.txt')
        evil_blob = git(repo, 'hash-object', '-w', '--stdin', stdin=b'REPLACED\n')
        git(repo, 'replace', orig_blob, evil_blob)
        check(git(repo, 'cat-file', 'blob', orig_blob) == 'REPLACED', 'test setup: replace ref must be effective')
        done = export(repo, sha, tmp / 'export-replace')
        check(done.returncode == 0, 'export with a replace ref failed', done.stderr)
        check((tmp / 'export-replace' / 'dir' / 'a.txt').read_text(encoding='utf-8') == 'a\n',
              'a refs/replace object rewrote the exported content (GIT_NO_REPLACE_OBJECTS missing)')

    # The relay must use the exporter rather than `git archive`.
    relay = (ROOT / 'tools' / 'ci' / 'arm64-cross-relay.sh').read_text(encoding='utf-8')
    check('relay-export-tree.py' in relay and 'git -C "$repo_root" archive' not in relay,
          'the relay must export the PR head with relay-export-tree.py, not git archive')
    print('relay export test: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
