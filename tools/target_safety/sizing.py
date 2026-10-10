# SPDX-License-Identifier: GPL-3.0-or-later
"""Topology-only sizing guard: complete protect, rechecks, drain and close only.

No inventory, content open, Git, package, prerequisite or acquisition call
exists in this module or its payload: the sizing bundle omits the inventory and
probes modules entirely. All counters come from metadata already collected
during coverage setup and identity rechecks; there is no unguarded census. The
completion event is distinct from the prerequisite final event and binds the
run to sizing mode, so a restored sizing window can never satisfy prerequisite
or ceiling acceptance. Every guard record goes through the bounded
diagnostic-write discipline: the guard_init and watch_session stage brackets
localize a stall inside the collected child transcript, and a broken channel
stops further diagnostics without ever aborting the window or its cleanup.
"""
from __future__ import annotations

import json
import time
from typing import Final

from . import bootstrap
from .coverage import (EXTERNAL, NAMESPACE_LINKS, NAMESPACE_TEXT, REPOSITORY, Scope, protect,
                       sqlite_absence_recheck)
from .egress import writer
from .kernel import Blocked, Watch, WINDOW_SECONDS
from .resource import ResourceError

MODE: Final = "topology-sizing"
COMPLETION_EVENT: Final = "topology_sizing_complete"
GUARD_STREAM_SECONDS: Final = WINDOW_SECONDS + 60.0


def main() -> int:
    """Complete one bounded topology-only sizing window; report only counters."""
    stream = writer()
    stream.rebind(time.monotonic() + GUARD_STREAM_SECONDS)
    watch: Watch | None = None
    try:
        stream.stage_started("guard_init")
        stream.record(bootstrap.LIMIT_READY_EVENT, bootstrap.child_limit_ready())
        stream.stage_completed("guard_init")
        stream.stage_started("watch_session")
        with Watch.session() as active:
            watch = active
            scope = Scope(REPOSITORY, EXTERNAL, REPOSITORY / ".codegraph", namespace_only=True)
            covered = protect(active, scope)
            active.check()
            if covered.sqlite_absence is not None:
                sqlite_absence_recheck(active, scope, covered.sqlite_absence)
            descriptors = len(active.paths)
        stream.stage_completed("watch_session")
        stream.record(COMPLETION_EVENT, {
            "mode": MODE,
            "discovered": covered.discovered,
            "processed": covered.processed,
            "pending": covered.pending,
            "kinds": {"directories": covered.directories,
                      "regular_files": covered.regular_files,
                      "symlinks": covered.symlinks},
            "watch_descriptors": descriptors,
            "path_bytes": covered.path_bytes,
            "regular_file_bytes": covered.regular_file_bytes,
            "roots": [str(REPOSITORY), str(EXTERNAL)],
            "namespace_only": [str(link) for link in NAMESPACE_LINKS],
            "literal_link_text": NAMESPACE_TEXT,
            "sqlite_absence": covered.sqlite_absence,
            "cleanup": {"watch_closed": active.closed, "descriptor_count": descriptors,
                        "fd_closed": active.closed},
            "inventory_complete": False,
            "acquisition": False,
            "task5_completed": False,
        })
        return 0
    except (OSError, Blocked, ResourceError) as error:
        if watch is not None:
            stream.record("watch_cleanup", json.dumps({"closed": watch.closed, "watches": len(watch.paths)}))
        stream.record("guard_blocked", str(error))
        return 70
