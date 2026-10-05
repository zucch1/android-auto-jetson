# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
# Run: python3 -B tests/target_safety/receipt.py
"""Prove receipt reservation precedes transport without running SSH."""
from __future__ import annotations

import json
import sys
import tempfile
from contextlib import ExitStack
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from target_safety import stage1


def fake_run(transports: list[tuple[str, ...]], fixture: Path, exit_status: int = 0):
    """Fake only the process boundary; record transport calls against real argv."""

    def run(argv: tuple[str, ...], payload: bytes | None = None) -> stage1.Receipt:
        if argv == stage1.SSH:
            transports.append(argv)
        return stage1.Receipt(argv=list(argv), command="fixture", cwd=str(fixture),
                              exit_status=exit_status, stdout="", stderr="")
    return run


def scenario(existing: bool) -> None:
    """Given a destination, When qualified, Then reserve before any transport."""
    fixture = Path(tempfile.mkdtemp(prefix="receipt-", dir="/tmp/opencode"))
    destination = fixture / "new.json"
    historical = fixture / "historical.json"
    historical.write_bytes(b"historical")
    if existing:
        destination.write_bytes(b"original")
    transports: list[tuple[str, ...]] = []

    with patch.object(stage1, "run", fake_run(transports, fixture)):
        try:
            with ExitStack() as stack:
                stage1.attempt(stack, destination)
        except FileExistsError:
            assert existing and not transports
            assert destination.read_bytes() == b"original"
        else:
            assert not existing and len(transports) == 1
            written = json.loads(destination.read_text())
            assert written["effective_caps"] == dict(stage1.EFFECTIVE_CAPS)
            assert written["resource_claim_limitations"] == stage1.RESOURCE_CLAIM_LIMITATIONS
    assert historical.read_bytes() == b"historical"
    print(f"PASS receipt exclusive existing={existing}; fixture={fixture}")


def qualification_failure() -> None:
    """Given failing local qualification, When attempted, Then zero transport calls."""
    fixture = Path(tempfile.mkdtemp(prefix="receipt-qual-", dir="/tmp/opencode"))
    destination = fixture / "new.json"
    transports: list[tuple[str, ...]] = []

    with patch.object(stage1, "run", fake_run(transports, fixture, exit_status=1)):
        with ExitStack() as stack:
            status = stage1.attempt(stack, destination)
    assert status == 1
    assert not transports
    assert not destination.exists()
    print(f"PASS receipt qualification-failure zero transports; fixture={fixture}")


def capacity_destination_allowlist() -> None:
    """Given the capacity receipt name, When selected, Then reserve exclusively before transport."""
    fixture = Path(tempfile.mkdtemp(prefix="receipt-capacity-", dir="/tmp/opencode"))
    historical = fixture / "task-5-prerequisites-expanded.json"
    historical.write_bytes(b"historical")
    transports: list[tuple[str, ...]] = []
    expected = fixture / "task-5-prerequisites-capacity.json"
    with patch.object(stage1, "run", fake_run(transports, fixture)), \
            patch.object(stage1, "EVIDENCE", fixture):
        with patch.object(sys, "argv", ["stage1.py", "--receipt", str(fixture / "not-allowed.json")]):
            try:
                stage1.main()
            except stage1.ReceiptDestinationError as error:
                assert error.arguments == ["--receipt", str(fixture / "not-allowed.json")]
            else:
                raise AssertionError("non-allowlisted destination accepted")
        assert not transports
        with patch.object(sys, "argv", ["stage1.py", "--receipt", str(expected)]):
            assert stage1.main() == 0
    assert len(transports) == 1
    written = json.loads(expected.read_text())
    assert written["effective_caps"] == dict(stage1.EFFECTIVE_CAPS)
    assert written["resource_claim_limitations"] == stage1.RESOURCE_CLAIM_LIMITATIONS
    assert historical.read_bytes() == b"historical"
    print(f"PASS receipt capacity allowlist exclusive reservation; fixture={fixture}")


def main() -> int:
    scenario(False)
    scenario(True)
    qualification_failure()
    capacity_destination_allowlist()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
