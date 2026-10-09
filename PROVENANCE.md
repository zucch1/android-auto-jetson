# Pinned protocol-source provenance

## Source identities

| Source | Commit | Original root tree | Use |
|---|---|---|---|
| [OpenCarDev/AASDK](https://github.com/opencardev/aasdk.git) | `9bf6adf933665dee26532201719fac14a047ccf1` | `19fbb172a1f7390e4f6618018029de8b7f10c3b0` | Entire tracked source snapshot in `third_party/aasdk/` |
| [mrmees/open-android-auto](https://github.com/mrmees/open-android-auto.git) | `61eab61c5f9968154ff1a80faa8c0a427b208479` | `332f633e14dc23bbe4227eb3bde6242d0878fb00` | 248 `oaa/**/*.proto` files plus original `LICENSE` and `README.md`, reference-only |

The OAA directory is
`third_party/reference/open-android-auto/61eab61c5f9968154ff1a80faa8c0a427b208479/`.
It is not OpenAuto. No OAA code generation, compilation, include path or overlay
is enabled. Its upstream README includes general compilation examples; these
are not instructions for this receiver's reference-only tree.

## Authority and audit data

`third_party/provenance/{aasdk,oaa}.ls-tree` contains the complete original
`git ls-tree -r -l PIN` output, including paths, modes, original blob IDs and
sizes for excluded files. Their SHA-256 digests are anchored in
`tools/provenance/inventory.py`. The checker hashes each retained file using
Git's raw `blob <size>\0<bytes>` identity and checks its mode and size against
these original listings. Thus upstream identity is independent of the
self-written `inventory.json`. Every byte, including all upstream headers,
must match. An invented AASDK root LICENSE, symlink, omitted file, extra file,
OAA overlay or generated output in either source tree fails the gate.

`inventory.json` is a deterministic derived report: original blob IDs, SHA-256
digests, all matching upstream notice lines, aggregate copyright lines and
every excluded upstream path with a reason. It is regenerated in memory and
compared byte-for-byte during checking; changing the report cannot authorize
a source change. No excluded capture, APK or phone-data content is published.
OAA audit YAML sidecars and all other non-selected files are excluded, including
the entire `analysis/`, `captures/` and `research/` trees (nested archives too).
The excluded analysis symlink is inventoried with its original Git mode and
blob identity; it is not imported into the retained reference subset.

## Reproduction

Use Git object databases fetched from the URLs above, not working-tree files:

```bash
set -euo pipefail
GIT_MASTER=1 git -C "$AASDK_REPO" rev-parse '9bf6adf933665dee26532201719fac14a047ccf1^{tree}'
GIT_MASTER=1 git -C "$AASDK_REPO" ls-tree -r -l 9bf6adf933665dee26532201719fac14a047ccf1 > aasdk.ls-tree
GIT_MASTER=1 git -C "$AASDK_REPO" archive 9bf6adf933665dee26532201719fac14a047ccf1 > aasdk.tar
GIT_MASTER=1 git -C "$OAA_REPO" rev-parse '61eab61c5f9968154ff1a80faa8c0a427b208479^{tree}'
GIT_MASTER=1 git -C "$OAA_REPO" ls-tree -r -l 61eab61c5f9968154ff1a80faa8c0a427b208479 > oaa.ls-tree
# Archive only LICENSE, README.md and the 248 oaa/**/*.proto paths in that listing.
# Pass the selected literal paths after -- to git archive; do not export research.
python3 -B tools/provenance/inventory.py "$PWD" > inventory.candidate.json
tools/provenance/check.sh
cmake -S . -B build/provenance
ctest --test-dir build/provenance -R provenance_isolation --output-on-failure
```

Git archive was used to reconstruct the selected OAA subset in this recovery;
the retained AASDK snapshot is independently verified against original blobs.
Fresh Git-listing reproduction and command exits are recorded in root task-2
evidence. Private mutation fixtures are retained with unique temporary paths.

## Downstream patch seam (task 3, not applied)

Both retained snapshots are unmodified; `downstream_patches` is empty. Task 3
must keep original blob/listing identities and record any GoogleTest CMake
pinning patch as a separately reviewable patch with path, original blob ID,
patch SHA-256, resulting blob ID and resulting SHA-256. The task-2 checker
currently rejects all changed upstream bytes; do not regenerate the original
listing or silently bless downstream hashes to bypass it.

## License gate and limits

See `THIRD_PARTY_NOTICES.md`. Objective redistribution gate: exact retained
upstream notices, original OAA LICENSE, byte-identical canonical GPL-3.0 text
outside the AASDK upstream root, documented missing AASDK root LICENSE,
explicit public reference credential notice and owner-signoff fallback rule.
This is a provenance/notice gate, not an independent legal opinion.

Only CMake/CTest provenance isolation is configured. No AASDK ABI compilation
has been performed; missing Boost/generated-header clangd diagnostics remain
unresolved upstream build inputs for task 7. There is no TLS hardening claim.
