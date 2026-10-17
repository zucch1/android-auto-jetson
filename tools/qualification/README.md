# Wired evidence export contract (task33)

These offline, stdlib-only tools **do not perform qualification** or change the
canonical evidence. Tasks31/32 still require actual live acceptance. The fixture
generator under `tests/qualification/` produces synthetic test data only. Never
publish its output as `docs/qualification/wired.json`.

```sh
python3 -B tools/qualification/check_wired.py PACKAGE/wired.json
python3 -B tools/qualification/check_privacy.py PACKAGE
python3 -B -m unittest discover -s tests/qualification -v
```

Both tools exit 0 on their stated checks passing, 1 on invalid/missing/unreadable
evidence, and 2 on CLI usage errors. Diagnostics deliberately withhold filenames,
matched text and JSON values, so rejection cannot leak the detected material.
Use a dedicated export directory: privacy validation scans **every file** below
the manifest's directory, including unreferenced files. Do not point it at a
repository or private raw-log directory.

## Manifest `aa-wired-qualification/1`

All fields below are required. Unknown top-level fields and unknown version /
topology fields fail. JSON duplicate keys and non-finite numbers fail. Types are
not coerced; booleans cannot stand in for numbers. Text must be nonblank, not
`unknown`, `pending`, `tbd` or `n/a` (case-insensitive).

| Field | Required content |
| --- | --- |
| `schema` | exactly `aa-wired-qualification/1` |
| `target` | text `model`, `jetpack_version`, `l4t_version`, `kernel_version` |
| `phone` | text `model`, `android_version`, `android_auto_version`; no device identity |
| `usb_topology` | text `controller`, `port`, `hub`, `cable`; use `direct` if no hub |
| `session_trace`, `redacted_log` | artifact references to nonempty sanitized text evidence |
| `task30`, `task31` | artifact references to native live harness result JSON |
| `perf_30m`, `soak_2h` | separate artifact references to native task32 result JSON |
| `limitations` | nonempty array of nonblank strings |
| `provenance` | task32-style deployment provenance described below |

Every artifact reference has exactly `path` (relative to the manifest directory)
and `sha256` (lowercase, unprefixed 64-hex digest of retained bytes). Files must
exist, be nonempty regular files, remain inside the package, and match the hash.
Absolute paths, `..`, symlinks and special files fail. Results must use four
distinct files; performance runs must have distinct generation timestamps.
Sanitize before hashing. Keep the original raw captures privately outside the
export; remove the optional `capture` pointer from sanitized task32 results so
private raw paths are not exported. Never invent missing historical bytes.

## Native result semantics

- Task30 requires its exact task/phase, PASS, no blocker, all four boolean
  observables true, and startup timing in 0..15000 ms (current harness ceiling).
  Both task30 and task31 ports must equal the manifest topology port.
- Task31 requires the exact live phase/task, full matrix (not a subset), no
  blocker, all six executable scenarios exactly once, PASS with their native
  observation tokens, nonnegative recovery strictly below the recorded budget
  (budget integer 1..5000 ms), elapsed time at least recovery time, nonnegative
  integer retries and frame baseline for media recovery. Unknown-phone rejection
  requires `attributed_rejection`; its native retry count may be null.
  `malformed_input` must be the explicit host-covered N/A row from the runbook;
  this does not claim a live malformed-input injection. Blocked/reduced-scope
  matrices are not accepted by version 1; owner-signoff support is not inferred.
- Task32 requires the exact task `32. Measure wired performance and two-hour soak`,
  validates against the existing perf schema, then independently
  checks live/COMPLETE_LIVE, PASS, empty gaps/violations/invalid telemetry, zero
  crashes, complete observed **and active** windows >=1800 / >=7200 seconds,
  positive media counters, connected/surviving display, orderly collector stop,
  no early exit/terminal/projection error and startup grace <=60 seconds.
  RSS <=250 MB, sustained CPU <=50% one core, A/V <=50 ms, audio <=200 ms and
  RSS growth <10% are recomputed from metric values, not metric labels. Gate
  flags, units, operators and per-metric budgets cannot weaken those limits.
  Helper RSS must be null/NOT_REQUIRED. Decode, frame-drop, IPC, RSS/CPU and
  thermal informational measurements must be numeric; thermal zones must be
  recorded. This does not reintroduce the withdrawn decode-p95 budget.
- Provenance requires SHA256 source/build/configuration digests, nonempty
  service definitions (name + digest), dependency versions, target hardware
  state, **`package_digest: null` and `equivalence_status: "provisional"`**.
  Both performance results must match the manifest provenance exactly.
  Release-package equivalence remains task43's responsibility.

## Privacy scope and limitations

Export files are limited to UTF-8 `.json`, `.jsonl`, `.ndjson`, `.txt`, `.log`,
and `.md`. Capture/credential filenames, opaque files, symlinks, binary data,
common pcap/H264 magic, capture schema markers, private-key/certificate blocks,
authorization material, credential-bearing URLs, MACs, coordinate pairs and
named USB serial/identity/location/credential assignments are rejected.
JSON escapes are decoded before scanning JSON documents/lines. Recognized
sanitized assignment values are `[REDACTED]`, `redacted`, `null`, native `id-`
pseudonyms followed by exactly 16 lowercase hex characters, and `sha256:` followed
by 16..64 lowercase hex characters. The entire value must match, including inside
quotes; a safe prefix followed by additional text is rejected. Unquoted text
assignments extend to the end of the line; put separate fields on separate lines.

This is a conservative known-pattern gate, **not universal secret detection**.
Unlabelled identifiers, novel encodings, arbitrary prose secrets and renamed
textual captures may evade patterns. Hashes bind bytes, not truth or authenticity;
the checker cannot prove a run occurred, distinguish fabricated measurements,
or prove two independently recorded runs from timestamps alone. Manual privacy
review and source evidence review remain mandatory. Static checks do not replay
private captures, remeasure hardware, or prove log/result correspondence.
Record completion-endpoint limitations (not photon/speaker observations),
withdrawn decode/fallback claims, host-only malformed-input coverage, and any
other actual limitations honestly in the package. A list's existence is checked,
not the factual completeness of its prose.

## CTest integration

The host-only `wired_qualification` test runs this offline suite from CTest with
a 120-second timeout. No target access, network, C++ rebuild or live evidence is
needed for this test. It does not certify a real qualification package.
