# SPDX-License-Identifier: GPL-3.0-or-later
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/ceiling_capture.py /tmp/opencode/<existing-unique-QA-directory>
"""Capture LOCAL qualification and source/history hashes; never write target evidence."""
from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import stage1


def main() -> int:
    capture = Path(sys.argv[1]).resolve()
    assert capture.is_relative_to(Path("/tmp/opencode")) and capture.is_dir()
    attempt = stage1.EVIDENCE / "task-5-prerequisites-ceiling-policy-durable-resume.json"
    durable = (attempt, *(attempt.with_suffix(suffix) for suffix in ('.remote.stdout', '.remote.stderr', '.lifecycle.jsonl')))
    summary = stage1.EVIDENCE / 'task-5-durable-prerequisite-attempt-summary.json'
    recorded: dict[Path, str] = {}
    for artifact in json.loads(summary.read_text())['artifacts']:
        assert isinstance(artifact['path'], str) and isinstance(artifact['sha256'], str)
        path, digest = Path(artifact['path']), artifact['sha256']
        if path in durable:
            assert path not in recorded, 'duplicate durable artifact'
            recorded[path] = digest
    assert set(recorded) == set(durable), 'summary must record receipt and all three sidecars'
    assert all(hashlib.sha256(path.read_bytes()).hexdigest() == digest
               for path, digest in recorded.items()), 'durable history hash mismatch before QA'
    resumed = stage1.EVIDENCE / "task-5-prerequisites-ceiling-policy-resumed.json"
    assert resumed.is_file() and resumed.read_bytes() == b'', "empty interrupted receipt must be preserved"
    policy = stage1.EVIDENCE / "task-5-prerequisites-ceiling-policy.json"
    assert policy.is_file(), "historical policy receipt must be preserved"
    retry = stage1.EVIDENCE / "task-5-prerequisites-ceiling-retry.json"
    assert retry.is_file(), "historical retry receipt must be preserved"
    historical = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in sorted(stage1.EVIDENCE.iterdir()) if path.is_file()}
    sources = stage1._source_checks()
    argv = (sys.executable, "-B", "tests/target_safety/ceiling_qa.py")
    started = time.monotonic()
    result = subprocess.run(argv, cwd=stage1.WORKTREE, capture_output=True,
                            check=False, timeout=120)
    elapsed = time.monotonic() - started
    for name, content in (("qualification.stdout", result.stdout), ("qualification.stderr", result.stderr)):
        with (capture / name).open("xb") as stream:
            stream.write(content)
    unchanged = all(Path(path).is_file() and hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest
                    for path, digest in historical.items())
    assert unchanged, 'historical bytes changed during QA'
    assert resumed.read_bytes() == b'' and retry.is_file() and policy.is_file()
    assert sources == stage1._source_checks(), "sources changed during qualification"
    output = result.stdout.decode()
    qa = {
        "timestamp_utc": datetime.now(timezone.utc).isoformat(),
        "argv": list(argv), "cwd": str(stage1.WORKTREE), "exit_status": result.returncode,
        "elapsed_seconds": elapsed,
        "pass_lines": sum(line.startswith("PASS ") for line in output.splitlines()),
        "rawQA": {name: {"path": str(capture / name), "sha256": hashlib.sha256(content).hexdigest()}
                  for name, content in (("qualification.stdout", result.stdout), ("qualification.stderr", result.stderr))},
        "source_checks": sources,
        "historical_receipt_hashes": historical, "historical_unchanged_during_QA": unchanged,
        "durable_receipt_and_sidecars_preserved": all(historical[str(path)] == digest
                                                    for path, digest in recorded.items()),
        "durable_recorded_hashes": {str(path): digest for path, digest in recorded.items()},
        "historical_empty_resumed_receipt_preserved": resumed.read_bytes() == b'',
        "historical_policy_receipt_preserved": policy.is_file(),
        "historical_retry_receipt_preserved": retry.is_file(),
        "production_bundle_sha256": hashlib.sha256(stage1.supervisor_bundle()).hexdigest(),
        "production_guard_bundle_sha256": hashlib.sha256(stage1.bundle()).hexdigest(),
        "effective_caps": stage1.EFFECTIVE_CAPS,
        "diagnostics": "LSP requested: basedpyright unavailable, previously declined; AST passes; no type/lint pass claimed",
    }
    with (capture / "qualification.json").open("x") as stream:
        json.dump(qa, stream, indent=2)
    print(json.dumps({"exit_status": result.returncode, "elapsed_seconds": elapsed,
                      "pass_lines": qa["pass_lines"], "rawQA_manifest": str(capture / "qualification.json")}))
    return result.returncode


if __name__ == "__main__":
    raise SystemExit(main())
