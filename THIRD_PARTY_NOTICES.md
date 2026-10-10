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

## Per-component license evidence

Header-coverage counts were re-verified by a full file scan of the pinned
trees on 2026-10-10. The earlier distribution-gate arithmetic
("200 + 278 = 478 of 566") double-cast the headerless set as only the 254
compiled schema files plus the 24 unit-test files and left 88 files
unclassified; the true, complete classification of all 566 upstream-tracked
AASDK files is **200 + 4 + 362 = 566** (per-file grant + in-file BSD + no
notice), stated below.

| Component | Version scope | Evidence path | Header coverage (2026-10-10 scan) |
|---|---|---|---|
| Project-owned code (`src/`, `include/`, `tools/`, `tests/`, `cmake/`) | GPL-3.0-or-later | root `LICENSE`; per-file `SPDX-License-Identifier: GPL-3.0-or-later` | 189 of 397 code files carry the header; remainder covered by the root README/License statement (per-file backfill tracked as r3) |
| AASDK per-file grant set | GPL-3.0-or-later ("either version 3 … or any later version") | e.g. `third_party/aasdk/src/Channel/Bluetooth/BluetoothService.cpp:1-16` | 200 of 566 tracked files (include/ 119, src/ 73, aasdk_proto/ 5, plus `Dockerfile`, `.github` script, `cmake_modules/DebPackageFilename.cmake`) |
| AASDK in-file BSD modules | BSD-3-Clause (GPL-3-compatible, build-only) | `third_party/aasdk/cmake_modules/{Findlibusb-1.0.cmake,CodeCoverage.cmake}` and `cmake_modules_old/` copies, in-file text | 4 of 566 |
| AASDK headerless material | no per-file grant; covered under upstream project-level `Readme.md` "GNU GPLv3" statement on current interpretation; later-version permission **not established** | `third_party/aasdk/protobuf/aap_protobuf/**/*.proto` (254, all compiled via `protobuf_generate_cpp`), `unit_test/` (24), build/meta/docs/scripts (84) | 362 of 566 |
| OAA reference subset | dual-noted (see Combined work licensing): per-file SPDX GPL-3.0-or-later preserved; aggregate "GPLv3" unresolved; distribution selects GPL version 3 | `oaa/**/*.proto:1` SPDX lines; vendored `LICENSE` (sha256 `3972dc97…`); vendored `README.md:239-243` | 240 of 248 `.proto` carry SPDX; 8 carry none (4 `RETRACTED` `oaa/input/` stubs, 3 `RETRACTED` `oaa/control/` stubs, `ChannelDescriptorData.proto`); + `LICENSE` + `README.md` = 250 files |
| Canonical GPL-3.0 text | FSF GPL-3.0, 29 June 2007 | root `LICENSE`; `third_party/LICENSES/GPL-3.0.txt` (both sha256 `3972dc97…`) | n/a |
| GoogleTest v1.15.2 (build/test, fetched not vendored) | BSD-3-Clause | `deps/manifest.json` pin (`b514bdc`, archive sha `7b42b4d6…`); upstream `LICENSE` at the pinned commit | n/a (test binaries link it statically) |
| Sanitized credential paths (removed 2026-10-10) | no third-party grant ever existed (finding c3) | `third_party/provenance/inventory.json` `downstream_patches`; `third_party/compat-credentials/README.md` | 0 copies retained (scanner-enforced) |

## Binary-distribution attribution (stub)

Attributions that apply when binaries or packages are distributed (host or
Jetson image; task 46 packaging). This is a stub with the verified component
set from `deps/manifest.json` and `toolchains/jetson-sysroot-manifest.json`;
it must be completed as a shipped `NOTICE`/attribution file before any binary
release:

| Component | License (as published upstream) | Where it attaches |
|---|---|---|
| GoogleTest v1.15.2 | BSD-3-Clause | test binaries (static) |
| OpenSSL | Apache-2.0 | linked (TLS) |
| Boost (system, log) | BSL-1.0 | linked |
| libusb-1.0 | LGPL-2.1-or-later | linked |
| GStreamer (+ plugins) | LGPL-2.1-or-later | linked/runtime |
| Qt6 | LGPL/GPL (per module) | optional runtime |
| protobuf | BSD-3-Clause | linked (generated + runtime) |
| glibc | LGPL-2.1-or-later | runtime (sysroot) |
| libstdc++ / GCC runtime | GPL-3.0-or-later with GCC Runtime Library Exception | runtime (sysroot) |
| nvidia-l4t-gstreamer | proprietary (NVIDIA license; optional) | optional runtime, not redistributed by this repo |



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

Project-owned code is GPL-3.0-or-later (root `LICENSE`). AASDK's headered
files carry per-file GPL-3.0-or-later grants; the headerless AASDK material
(including the compiled protobuf schemas) is covered under the upstream
project-level "GNU GPLv3" Readme statement on the current interpretation,
and a later-version permission for it is **not established**. The OAA
reference material is dual-noted: the explicit upstream per-file later-version
grants (**240 files carry `SPDX-License-Identifier: GPL-3.0-or-later`**) are
preserved and not relicensed, while the upstream aggregate (`README.md`
"GPLv3" plus the bare GPL-3.0 `LICENSE`) reads as GPL version 3 without
later-version wording. Because version scope is unresolved for parts of the
collection, **this distribution selects GPL version 3**, a version permitted
by every reading; no blanket "or any later version" assertion is made for
third-party material with unresolved version scope. The sources and original
notices of both vendored components are provided in-tree.
