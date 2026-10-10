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
must match except at the sanctioned sanitization paths recorded below. An invented AASDK root LICENSE, symlink, omitted file, extra file,
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

## Downstream patch seam and sanitization record

### Effective-stage patches (applied to a staged copy, never to these trees)

`deps/patches/aasdk-googletest.patch` (unified diff) and
`deps/patches/aasdk-tls-credentials.patch` (`aa-locked-source-patch/1`
digest-anchored JSON recipe) are applied only to the content-addressed
effective build stage (`tools/deps/stage.py`); pristine vendored sources are
never edited in place. Both patches keep original blob/listing identities and
are recorded as separately reviewable artifacts with path, original SHA-256
and patch SHA-256 bound in `deps/manifest.lock`.

### Sanitization transformation (2026-10-10) — the vendored tree no longer byte-matches upstream

The owner-approved license remediation removed every published copy of the
historical head-unit credential from the shipped tree. The upstream ls-tree
listings stay untouched as the authority for original identities; the
sanctioned downstream transformation is recorded in
`third_party/provenance/inventory.json` (`downstream_patches`) and enforced by
`tools/provenance/check.py`. Format: path, original blob ID, original
SHA-256, transformation ID, resulting blob ID and resulting SHA-256 (or
`deleted`). Do not regenerate the original listing and do not bless further
downstream hashes without a new sanctioned record.

| Path | Original blob / sha256 | Transformation | Result |
|---|---|---|---|
| `third_party/aasdk/cert/headunit.crt` | `45ad6cc4fd9f…` / `85b5043a09b1…` | `delete-credential-copy-2026-10-10` | deleted |
| `third_party/aasdk/cert/headunit.key` | `c2b2666a8021…` / `9e837a172a1e…` | `delete-credential-copy-2026-10-10` | deleted |
| `third_party/aasdk/src/Messenger/Cryptor.cpp` | `71c679ff32e1…` / `af6d9f58d135…` | `strip-embedded-credential-2026-10-10` | blob `cce822d47b7e…`, sha256 `6c895a8ea672…` |
| `third_party/compat-credentials/headunit.crt` | `45ad6cc4fd9f…` / `85b5043a09b1…` | `delete-credential-copy-2026-10-10` | deleted |
| `third_party/compat-credentials/headunit.key` | `c2b2666a8021…` / `9e837a172a1e…` | `delete-credential-copy-2026-10-10` | deleted |

`src/Messenger/Cryptor.cpp` keeps its upstream GPL-3.0-or-later header
unchanged; the edit strips the embedded `cCertificate`/`cPrivateKey` string
literals and replaces the credential-acquisition block with a fail-closed,
`HU_KEY_PATH`-only loading path (`aa::tls::load_credentials()`), loader
semantics otherwise unchanged. This is a documented downstream modification
of an upstream GPL-3.0-or-later work (see the §5(a) dated notices in
`deps/patches/`); it is **not** claimed to be upstream-identical. The two
compat-credentials deletions cover project-added duplicate copies of the same
bytes (not upstream-tracked). Historical copies remain in Git history up to
`57f4045ebf2a…` (known exposure, open counsel question; see
`third_party/compat-credentials/README.md`).

The checker rejects any other divergence (`BLOB_MISMATCH`), any restored
sanitized path (`SANITIZED_FILE_RESTORED`) and any altered resulting identity
(`SANITIZATION_MISMATCH`).

## License gate and limits

See `THIRD_PARTY_NOTICES.md`. Objective redistribution gate: exact retained
upstream notices, original OAA LICENSE, byte-identical canonical GPL-3.0 text
outside the AASDK upstream root, documented missing AASDK root LICENSE,
explicit public reference credential notice and owner-signoff fallback rule.
This is a provenance/notice gate, not an independent legal opinion.

Only CMake/CTest provenance isolation is configured. No AASDK ABI compilation
has been performed; missing Boost/generated-header clangd diagnostics remain
unresolved upstream build inputs for task 7. There is no TLS hardening claim.
