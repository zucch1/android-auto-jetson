# Credential disposition note (2026-10-10)

**The compatibility credential previously kept here was removed from this
distribution.** No certificate or private key bytes are committed in this
repository anymore, in this directory or anywhere else.

## What was removed

On 2026-10-10 the owner-approved license remediation deleted every published
copy of the historical public Android Auto head-unit credential:

- `third_party/compat-credentials/headunit.crt` and `headunit.key` (this
  directory),
- `third_party/aasdk/cert/headunit.crt` and `headunit.key`,
- the embedded `cCertificate` / `cPrivateKey` string literals in
  `third_party/aasdk/src/Messenger/Cryptor.cpp` (replaced with a fail-closed,
  `HU_KEY_PATH`-only loading path).

Exact removal identities (original Git blob IDs and SHA-256 digests) are
recorded in `third_party/provenance/inventory.json` under
`downstream_patches`, and the transformation is described in `PROVENANCE.md`.
`tools/tls/scan.py` now enforces zero credential copies with no allowlist.

## Why it was removed

The certificate was issued to a third-party identity (JVC Kenwood, under
Google Automotive Link) and no standalone redistribution grant from the
certificate subject or issuer exists. The distribution-gate audit of
2026-10-10 recorded this as finding c3 (terms technically unclear) and the
owner chose staged removal over continued redistribution. A responsible
disclosure draft recommending key rotation/denylist is filed with the
remediation receipt (`.omo/evidence/jetson-android-auto-receiver/`).

## Known historical exposure (open counsel item)

The same bytes remain in this repository's Git history at commits up to and
including `57f4045ebf2a74064a27cbfab8441154c170b26f`, and in upstream
`opencardev/aasdk@9bf6adf` (`src/Messenger/Cryptor.cpp` literals and
`cert/`). Per the owner decision of 2026-10-09 ("forward-only + counsel
review") history is not rewritten; the exposure is documented as a known
exposure with an open counsel question on whether historical remediation is
required before any public release or tag.

## Current posture

- Production identity is operator-supplied through `HU_KEY_PATH` only; the
  loader fails closed (see `docs/tls-credentials.md`).
- Test material is a generated synthetic, neutral-DN, self-signed credential
  (`tools/tls/synthetic_credential.py`); it is never committed and is not a
  third-party identity.
- Upstream copyright and GPL-3.0-or-later terms apply to the remaining
  upstream content; see `THIRD_PARTY_NOTICES.md`.
