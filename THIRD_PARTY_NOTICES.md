# Third-Party Notices

This repository vendors two upstream projects as pinned, unmodified source
snapshots. This file is the license gate record consumed by
`tools/provenance/check.sh`; the `provenance-fact:` tokens below are
machine-checked structural facts, not prose.

## AASDK (OpenCarDev) — `third_party/aasdk/`

- Upstream: <https://github.com/opencardev/aasdk.git>
- Pinned commit: `9bf6adf933665dee26532201719fac14a047ccf1`
- Upstream tree: `19fbb172a1f7390e4f6618018029de8b7f10c3b0`
- Vendoring strategy: exact tracked snapshot (566 tracked files, 564
  retained, 2 credential files deleted by the sanitization below); every
  retained byte except the single documented `src/Messenger/Cryptor.cpp`
  credential-stripping edit matches upstream exactly. The downstream
  transformation record lives in `third_party/provenance/inventory.json`
  (`downstream_patches`) and `PROVENANCE.md`.
- Declared license: GPL-3.0-or-later, per per-file headers ("either version 3
  of the License, or (at your option) any later version") and the upstream
  `Readme.md` ("GNU GPLv3", Copyright (c) 2018 f1x.studio (Michal Szwaj);
  per-file headers also credit OpenCarDev Team 2025).

### Missing upstream root LICENSE

- provenance-fact: aasdk-root-license-missing
- The pinned upstream commit contains **no** root `LICENSE` or `COPYING`
  file, although `Readme.md` links to one ("See [LICENSE](LICENSE) for
  details"). This is an upstream gap, preserved as-is.
- This repository does **not** invent `third_party/aasdk/LICENSE`; the
  checker fails with `INVENTED_LICENSE_PRESENT` if one appears.
- The canonical GPL-3.0 text is provided instead at
  `third_party/LICENSES/GPL-3.0.txt` (byte-identical copy of this
  repository's root `LICENSE`, SHA-256
  `3972dc9744f6499f0f9b2dbf76696f2ae7ad8af9b23dde66d6af86c9dfb36986`),
  outside the upstream tree path, without fabricating upstream provenance.

### License-gate fallback rule (not applied)

- provenance-fact: fallback-f1xpl-aasdk-9ee5283245a630afce3a441696b9dde484ecbcd9
- If the license review of `opencardev/aasdk@9bf6adf` ever fails (for
  example the missing root LICENSE is judged to make redistribution
  unclear), the documented fallback is **`f1xpl/aasdk`
  `9ee5283245a630afce3a441696b9dde484ecbcd9`** (upstream origin of the
  aasdk codebase).
- **A fallback switch is never silent.** It requires explicit owner
  sign-off recorded in the task evidence
  (`.omo/evidence/jetson-android-auto-receiver/task-2-provenance.json`
  successors), plus a new inventory, new pin documentation, and re-run of
  `tools/provenance/check.sh`. As of this record the gate result for
  `9bf6adf` is **pass-with-documentation** and the fallback is **not**
  applied.

### Head-unit credential removed (previously published, now sanitized)

- provenance-fact: headunit-credential-removed-2026-10-10
- The historical public Android Auto head-unit credential (an X.509
  certificate issued to JVC Kenwood under Google Automotive Link, and its
  private key) was published in this repository in three copies and **was
  removed from the distribution on 2026-10-10** by the owner-approved license
  remediation: `third_party/aasdk/cert/headunit.{crt,key}`,
  `third_party/compat-credentials/headunit.{crt,key}`, and the embedded
  `cCertificate`/`cPrivateKey` string literals in
  `third_party/aasdk/src/Messenger/Cryptor.cpp` (replaced with a fail-closed,
  `HU_KEY_PATH`-only loading path).
- No standalone license grant from the certificate subject or issuer ever
  existed in-tree or upstream; distribution relied on the artifact's public
  compatibility-reference character, which the owner withdrew on
  2026-10-09/10 (audit finding c3). See
  `third_party/compat-credentials/README.md` for the disposition record.
- **Known historical exposure (open counsel item):** the same bytes remain in
  this repository's Git history up to commit `57f4045ebf2a74064a27cbfab8441154c170b26f`
  and in upstream `opencardev/aasdk@9bf6adf`. History is not rewritten (owner
  decision: forward-only + counsel review). A responsible-disclosure draft
  recommending credential rotation/denylist is filed with the remediation
  receipt.
- The committed marker scanner (`tools/tls/scan.py`) now allowlists nothing:
  any private-key PEM marker, any file named `headunit.key`/`headunit.crt`,
  and any byte-identical copy of the removed material fail the scan. Tests
  generate a clearly labeled synthetic, neutral-DN, self-signed credential
  (`tools/tls/synthetic_credential.py`) and never commit it.
- No TLS hardening claim is made here; transport/credential posture is
  documented in `docs/tls-credentials.md`.

## Open Android Auto (mrmees) — `third_party/reference/open-android-auto/61eab61c5f9968154ff1a80faa8c0a427b208479/`

- Upstream: <https://github.com/mrmees/open-android-auto.git>
  (**not** the unrelated `OpenAuto`/`f1xpl/openauto` project)
- Pinned commit: `61eab61c5f9968154ff1a80faa8c0a427b208479`
- Upstream tree: `332f633e14dc23bbe4227eb3bde6242d0878fb00`
- Vendoring strategy: audited schema reference subset (2640 tracked,
  250 vendored: 248 `oaa/**/*.proto` schemas plus `LICENSE` and `README.md`;
  2390 individually inventoried exclusions). All other paths are excluded,
  including audit YAML sidecars, `analysis/`, `captures/` and all `research/`
  archives containing captures and extracted phone/APK material.
- Upstream `LICENSE` (GPL-3.0) is vendored verbatim inside the reference
  tree as upstream tracked content.

### Reference-only isolation

- provenance-fact: oaa-reference-only
- This tree is **reference-only protocol material** (message schemas with
  original license and README). It is never compiled, code-generated from, or overlaid
  onto `third_party/aasdk/`.
- Plan constraint: OAA schemas must not be overlaid with AASDK generated
  code, and no OAA file may appear inside the AASDK tree. The checker
  rejects extras in either tree with named diagnostics
  (`OAA_OVERLAY_IN_AASDK`, `GENERATED_OUTPUT_IN_AASDK`) and
  `tests/provenance/isolation.sh` proves the guard on a disposable fixture.

## Aggregate copyright inventory

Per-file retained upstream notice lines and SHA-256 / git-blob-oid content
identities for all 814 retained vendored files are recorded in
`third_party/provenance/inventory.json` (`copyright_lines` section
aggregates copyright-bearing header lines with counts); its
`downstream_patches` section is the sanitization-transformation record for
the 5 removed/edited paths. Header retention is gated by exact original raw
Git blob identity (`BLOB_MISMATCH`) in the checker; changing any header byte
fails even if the inventory is edited too.

## Combined work licensing

Project-owned code is GPL-3.0-or-later (root `LICENSE`). AASDK is
GPL-3.0-or-later; the OAA reference material remains GPL-3.0-only. Distribution
containing both can use GPL version 3, a version permitted by both licenses.
The reference material is not relicensed to permit later GPL versions. The
sources and original notices of both vendored components are provided in-tree.
