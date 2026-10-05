# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/snapshot_cli.py
"""CLI driver: arg parsing, ok summary over a fixture tree, blocked exit 70, manifest digest."""
from __future__ import annotations

import contextlib
import hashlib
import io
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from snapshot_fixtures import expected_hashed_bytes, run_cases

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety.snapshot_cli import CliArgs, parse
from target_safety.snapshot_cli import main as driver_main


def _drive(argv: list[str]) -> tuple[int, dict[str, object]]:
    """Run the driver with a captured stdout; require exactly one JSON line."""
    stream = io.StringIO()
    with contextlib.redirect_stdout(stream):
        code = driver_main(argv)
    lines = stream.getvalue().splitlines()
    assert len(lines) == 1, f"expected exactly one stdout line, got {lines!r}"
    return code, json.loads(lines[0])


def test_arg_parsing_builds_typed_inputs(tmp_path: Path) -> None:
    """Given a full argv with two roots and two exceptions, When parsing, Then the typed
    inputs carry root order, literal link targets (including one containing '=') and
    the identity strings."""
    args = parse([
        "--root", "/roots/one", "--root", "/roots/two",
        "--link-exception", "/roots/one/bin/python3=/usr/bin/python3",
        "--link-exception", "/roots/one/bin/odd=name=with=equals",
        "--manifest", str(tmp_path / "m.json"),
        "--task-id", "window-9", "--target-identity", "jetson.local",
    ])
    assert isinstance(args, CliArgs)
    assert args.roots == (Path("/roots/one"), Path("/roots/two"))
    assert [(exc.path, exc.expected_target) for exc in args.link_exceptions] == [
        (Path("/roots/one/bin/python3"), "/usr/bin/python3"),
        (Path("/roots/one/bin/odd"), "name=with=equals"),
    ]
    assert args.manifest == tmp_path / "m.json"
    assert args.task_id == "window-9" and args.target_identity == "jetson.local"
    print("PASS snapshot_cli parses roots, exceptions and identity into typed inputs")


def test_arg_parsing_rejects_malformed_input(tmp_path: Path) -> None:
    """Given malformed argv, When parsing, Then argparse exits 2 for a pair without '=',
    an empty path/target side, or a missing --root."""
    malformed = (
        ["--root", "/r", "--link-exception", "novalue", "--manifest", "/tmp/m.json"],
        ["--root", "/r", "--link-exception", "=target", "--manifest", "/tmp/m.json"],
        ["--root", "/r", "--link-exception", "path=", "--manifest", "/tmp/m.json"],
        ["--link-exception", "/r/x=/usr/bin/x", "--manifest", "/tmp/m.json"],
    )
    for argv in malformed:
        try:
            parse(argv)
        except SystemExit as error:
            assert error.code == 2, (argv, error.code)
            continue
        raise AssertionError(f"malformed argv accepted: {argv!r}")
    print("PASS snapshot_cli rejects malformed argv with exit 2")


def test_ok_summary_over_fixture_tree(tmp_path: Path) -> None:
    """Given a fixture tree with an approved external link, When driving the CLI, Then one
    ok summary line reports the real entry count, hashed bytes, zero errors, capabilities
    and the retained manifest."""
    tree = tmp_path / "tree"
    (tree / "empty").mkdir(parents=True)
    (tree / "keep.txt").write_bytes(b"hello\n")
    (tree / "soft").symlink_to("keep.txt")
    link = tree / "ext" / "venv" / "bin" / "python3"
    link.parent.mkdir(parents=True)
    link.symlink_to("/usr/bin/python3")
    manifest_path = tmp_path / "manifest.json"
    code, summary = _drive([
        "--root", str(tree),
        "--link-exception", f"{link}=/usr/bin/python3",
        "--manifest", str(manifest_path),
        "--task-id", "window-cli", "--target-identity", "local-fixture",
    ])
    assert code == 0
    assert summary["status"] == "ok"
    assert summary["entry_count"] == 8
    assert summary["bytes_hashed"] == expected_hashed_bytes(tree) == 30
    assert isinstance(summary["elapsed_seconds"], float) and summary["elapsed_seconds"] >= 0.0
    assert summary["traversal_errors"] == 0
    capabilities = summary["capabilities"]
    assert isinstance(capabilities, list) and len(capabilities) == 1
    assert isinstance(capabilities[0], dict)
    assert {"root", "xattrs", "acls", "inode_flags", "detail"} <= set(capabilities[0])
    assert summary["manifest_bytes"] == manifest_path.stat().st_size
    assert manifest_path.stat().st_size > 0
    print("PASS snapshot_cli ok summary reports counts, bytes, capabilities and manifest")


def test_manifest_digest_matches_file(tmp_path: Path) -> None:
    """Given an ok run, When checking the summary, Then manifest_sha256 is SHA-256 of the
    retained manifest file bytes and manifest_bytes is that file's size."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "keep.txt").write_bytes(b"hello\n")
    manifest_path = tmp_path / "manifest.json"
    code, summary = _drive([
        "--root", str(tree), "--manifest", str(manifest_path),
        "--task-id", "window-cli", "--target-identity", "local-fixture",
    ])
    assert code == 0 and summary["status"] == "ok"
    blob = manifest_path.read_bytes()
    assert summary["manifest_sha256"] == hashlib.sha256(blob).hexdigest()
    assert summary["manifest_bytes"] == len(blob)
    print("PASS snapshot_cli manifest digest matches the retained file bytes")


def test_unapproved_external_link_blocks_exit70(tmp_path: Path) -> None:
    """Given an external link with no approved exception, When driving the CLI, Then the
    single summary line is a blocked record with exit 70 and no manifest is produced."""
    tree = tmp_path / "tree"
    tree.mkdir()
    (tree / "evil").symlink_to("/etc/passwd")
    manifest_path = tmp_path / "manifest.json"
    code, summary = _drive([
        "--root", str(tree), "--manifest", str(manifest_path),
        "--task-id", "window-cli", "--target-identity", "local-fixture",
    ])
    assert code == 70
    assert summary["status"] == "blocked"
    assert summary["reason"] == "unapproved_external_link"
    assert "evil" in str(summary["detail"]) and "/etc/passwd" in str(summary["detail"])
    assert not manifest_path.exists() or manifest_path.read_bytes() == b"", \
        "a blocked run must leave no manifest (empty claim marker at most)"
    print("PASS snapshot_cli unapproved external link blocks with exit 70 and no manifest")


def main() -> int:
    return run_cases((
        test_arg_parsing_builds_typed_inputs,
        test_arg_parsing_rejects_malformed_input,
        test_ok_summary_over_fixture_tree,
        test_manifest_digest_matches_file,
        test_unapproved_external_link_blocks_exit70,
    ), "snapshot-cli-")


if __name__ == "__main__":
    raise SystemExit(main())
