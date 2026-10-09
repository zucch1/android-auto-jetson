"""Local driver for the task-5 target acquisition route (never runs on target).

Freezes the acquisition payload as a deterministic source tar of the
target_safety route modules (the snapshot producer with its 64 GiB bound and
CLI, the equivalence consumer, and the acquire route), computes its SHA-256,
loads the finite budget, and renders the exact one-transport extraction +
invocation for an independent reviewer. A source tar (not in-memory exec) is
required because the snapshot producer's tool digest reads its module sources
from disk via ``__file__``; extraction into a real package directory is the
documented bounded extraction step. This driver makes no target contact.
"""
from __future__ import annotations

import hashlib
import io
import json
import tarfile
from pathlib import Path
from typing import Final

HERE: Final = Path(__file__).resolve().parent
PACKAGE: Final = "target_safety"

# Exact route modules in the frozen payload: the snapshot producer (64 GiB
# bound in snapshot_models) and its CLI, the equivalence consumer, and acquire.
MODULES: Final = (
    "acquire", "acquire_input_select", "acquire_models", "acquire_readers", "kernel",
    "acquire_proc", "acquire_proc_helper", "acquire_transport_io",
    "snapshot", "snapshot_capture", "snapshot_cli", "snapshot_codec", "snapshot_diff",
    "snapshot_envelope", "snapshot_git", "snapshot_models", "snapshot_probe", "snapshot_walk",
)

DEFAULT_ROOTS: Final = (
    "/home/zucchi/Desktop/infotainment-plan",
    "/home/zucchi/.omo/codegraph/projects/infotainment-plan-68328f6064505b10",
)
DEFAULT_LINK_EXCEPTIONS: Final = (
    "/home/zucchi/Desktop/infotainment-plan/.local_env/aqt-venv/bin/python3=/usr/bin/python3",
    "/home/zucchi/Desktop/infotainment-plan/.venv/bin/python3=/usr/bin/python3",
)


def module_digests() -> dict[str, str]:
    """Explicit exact files/digests: per-module SHA-256 of source bytes."""
    return {f"{PACKAGE}/{name}.py": hashlib.sha256(
        (HERE / f"{name}.py").read_bytes()).hexdigest() for name in MODULES}


def module_sources() -> dict[str, bytes]:
    return {f"{PACKAGE}/{name}.py": (HERE / f"{name}.py").read_bytes() for name in MODULES}


def build_tar(members: dict[str, bytes]) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for name in sorted(members):
            data = members[name]
            info = tarfile.TarInfo(name)
            info.size = len(data)
            info.mtime = 0
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            info.mode = 0o644
            tar.addfile(info, io.BytesIO(data))
    return buffer.getvalue()


def acquire_payload() -> bytes:
    """Deterministic tar of the route sources plus the frozen input policy."""
    members = module_sources()
    members["target_safety/acquire-input-policy.json"] = (
        HERE / "acquire-input-policy.json").read_bytes()
    return build_tar(members)


def payload_digest() -> dict[str, object]:
    blob = acquire_payload()
    return {"payload_sha256": hashlib.sha256(blob).hexdigest(),
            "payload_bytes": len(blob)}


def load_budget() -> dict[str, object]:
    return json.loads((HERE / "acquire-budget.json").read_text(encoding="utf-8"))


def invocation(payload: str = "payload.tar",
               extract_dir: str = "/tmp/aa-acquire-payload",
               scratch: str = "/tmp/aa-acquire-window",
               window_id: str = "task-5-acquisition-window",
               target_identity: str = "jetson.local") -> str:
    """Extraction + one-transport command a later authorized window runs (live select)."""
    src = extract_dir
    return "\n".join([
        f"mkdir -p {src} {scratch}",
        f"tar -xf {payload} -C {src}",
        f"PYTHONPATH={src} python3 -B -m {PACKAGE}.acquire \\",
        f"  --scratch {scratch} \\",
        *[f"  --root {root} \\" for root in DEFAULT_ROOTS],
        *[f"  --link-exception {pair} \\" for pair in DEFAULT_LINK_EXCEPTIONS],
        f"  --task-window-id {window_id} \\",
        f"  --target-identity {target_identity}",
    ])


def policy_sha256() -> str:
    return hashlib.sha256((HERE / "acquire-input-policy.json").read_bytes()).hexdigest()


def summary() -> dict[str, object]:
    budget = load_budget()
    return {
        "payload": payload_digest(),
        "modules": list(MODULES),
        "module_digests": module_digests(),
        "budget_file": "tools/target_safety/acquire-budget.json",
        "budget_sha256": hashlib.sha256(
            (HERE / "acquire-budget.json").read_bytes()).hexdigest(),
        "input_policy_file": "tools/target_safety/acquire-input-policy.json",
        "input_policy_sha256": policy_sha256(),
        "total_budget_seconds": budget["total_budget_seconds"],
        "retry_policy": budget["retry_policy"],
        "invocation": invocation(),
    }


if __name__ == "__main__":
    print(json.dumps(summary(), indent=2, sort_keys=True))
