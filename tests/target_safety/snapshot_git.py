# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_git.py
"""Git supplement: read-only status/tracked/untracked capture and blocked external links."""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import entry_map, frozen_request, git_env, git_init, run_cases

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.snapshot import snapshot


def _git_record(manifest, rel: bytes):
    for record in manifest.git:
        if record.path == rel:
            return record
    raise AssertionError(f"no git record for {rel!r}: {[r.path for r in manifest.git]}")


def test_repo_supplement_captures_status_tracked_untracked(tmp_path: Path) -> None:
    """Given a repository with tracked, untracked and ignored files, When snapshotting,
    Then deterministic git lists are captured and the physical inventory stays authoritative."""
    home = tmp_path / "home"
    home.mkdir()
    tree = tmp_path / "tree"
    git_init(tree, home)
    (tree / "tracked.txt").write_bytes(b"t\n")
    (tree / "ignored.log").write_bytes(b"i\n")
    (tree / "untracked.txt").write_bytes(b"u\n")
    (tree / ".gitignore").write_text("ignored.log\n")
    env = git_env(home)
    subprocess.run(["git", "-C", str(tree), "add", "tracked.txt", ".gitignore"],
                   check=True, capture_output=True, env=env)
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    record = _git_record(manifest, b"")
    assert record.result == "ok" and record.git_kind == "directory"
    assert record.tracked == (b".gitignore", b"tracked.txt")
    assert record.untracked == (b"untracked.txt",)
    assert record.status == (b"?? untracked.txt", b"A  .gitignore", b"A  tracked.txt")
    assert tuple(name for name, _, _ in record.commands) == ("status", "tracked", "untracked")
    assert all(exit_status == 0 for _, exit_status, _ in record.commands)
    assert entry_map(manifest)[b"ignored.log"].sha256 is not None
    assert entry_map(manifest)[b".git/HEAD"].type == "file"
    print("PASS snapshot git supplement captures deterministic status/tracked/untracked")


def test_nested_repository_gets_own_record(tmp_path: Path) -> None:
    """Given a nested repository inside the protected root, When snapshotting, Then both
    the outer and nested repository produce their own supplemental record."""
    home = tmp_path / "home"
    home.mkdir()
    tree = tmp_path / "tree"
    git_init(tree, home)
    nested = tree / "nested"
    nested.mkdir()
    git_init(nested, home)
    (nested / "inner.txt").write_bytes(b"n\n")
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    assert [record.path for record in manifest.git] == [b"", b"nested"]
    assert _git_record(manifest, b"nested").result == "ok"
    assert entry_map(manifest)[b"nested/.git/HEAD"].type == "file"
    print("PASS snapshot nested repository produces its own supplemental record")


def test_external_gitdir_recorded_blocked_not_traversed(tmp_path: Path) -> None:
    """Given a .git file pointing at an external gitdir, When snapshotting, Then the repo
    record is a blocked result and the external location is never traversed."""
    home = tmp_path / "home"
    home.mkdir()
    external = tmp_path / "external-gitdir"
    external.mkdir()
    (external / "HEAD").write_text("ref: refs/heads/main\n")
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / ".git").write_text(f"gitdir: {external}\n")
    (tree / "work.txt").write_bytes(b"w\n")
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    record = _git_record(manifest, b"")
    assert record.result == "blocked" and record.reason == "external_gitdir"
    assert record.status == record.tracked == record.untracked == ()
    assert all(entry.path in (b"", b".git", b"work.txt") for entry in manifest.entries)
    assert entry_map(manifest)[b".git"].type == "file"
    print("PASS snapshot external gitdir records a blocked result without traversal")


def test_external_alternate_recorded_blocked_not_traversed(tmp_path: Path) -> None:
    """Given an alternates file pointing at an external object store, When snapshotting,
    Then the repo record is a blocked result and the external store is never traversed."""
    home = tmp_path / "home"
    home.mkdir()
    tree = tmp_path / "tree"
    git_init(tree, home)
    info = tree / ".git" / "objects" / "info"
    info.mkdir(parents=True, exist_ok=True)
    (info / "alternates").write_text("/tmp/opencode/external-object-store\n")
    (tree / "f.txt").write_bytes(b"f\n")
    manifest = snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    record = _git_record(manifest, b"")
    assert record.result == "blocked" and record.reason == "external_alternate"
    assert record.status == record.tracked == record.untracked == ()
    assert all(not entry.path.startswith(b"/") for entry in manifest.entries)
    print("PASS snapshot external alternate records a blocked result without traversal")


def test_git_commands_disable_index_writes_and_fsmonitor(tmp_path: Path) -> None:
    """Given any discovered repository, When the supplement runs, Then every git command
    disables optional index writes and fsmonitor, keeping the probe read-only."""
    home = tmp_path / "home"
    home.mkdir()
    tree = tmp_path / "tree"
    git_init(tree, home)
    (tree / "f.txt").write_bytes(b"f\n")
    captured: list[tuple[tuple[str, ...], dict[str, str]]] = []
    real_run = subprocess.run

    def recording_run(argv, **kwargs):
        captured.append((tuple(argv), dict(kwargs.get("env") or {})))
        return real_run(argv, **kwargs)

    import unittest.mock
    with unittest.mock.patch("target_safety.snapshot_git.subprocess.run",
                             side_effect=recording_run):
        snapshot(frozen_request((tree,)), tmp_path / "manifest.json")
    assert len(captured) == 3
    for argv, env in captured:
        assert argv[:3] == ("git", "-c", "core.fsmonitor=false")
        assert "-c" in argv and "core.untrackedCache=false" in argv
        assert env.get("GIT_OPTIONAL_LOCKS") == "0"
    print("PASS snapshot git commands disable index writes and fsmonitor")


def main() -> int:
    return run_cases((
        test_repo_supplement_captures_status_tracked_untracked,
        test_nested_repository_gets_own_record,
        test_external_gitdir_recorded_blocked_not_traversed,
        test_external_alternate_recorded_blocked_not_traversed,
        test_git_commands_disable_index_writes_and_fsmonitor,
    ), "snapshot-git-")


if __name__ == "__main__":
    raise SystemExit(main())
