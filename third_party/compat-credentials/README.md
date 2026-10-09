# Public upstream Android Auto compatibility reference

`headunit.crt` and `headunit.key` are the verbatim runtime PEM bytes from
Cryptor.cpp at AASDK 9bf6adf933665dee26532201719fac14a047ccf1. This is a
**public compatibility reference, not a production secret**. No runtime default
loads these files and no install target deploys them. Tests explicitly copy and
combine them into an owner-only file. Production credential provisioning is out
of scope; never commit a production private key.

HU_KEY_PATH must explicitly name a regular, effective-user-owned, single-link
0600 file containing the certificate followed by the unencrypted private key.
The final path component must not be a symlink. Provision parent directories
under operator control. Missing, unreadable, malformed, insecure or mismatched
credentials fail closed; diagnostics report only a failure class, never paths
or PEM contents.

TLS presents the receiver/head-unit credential to the phone and encrypts the
session; it does not establish phone approval. The real-phone mode is explicitly
`encryption-only-compatibility`. Phones are not assumed to present verifiable
peer certificates. A session requires the injected approved-phone predicate;
the absent predicate denies. Task 27 will supply its approved-phone store and
pairing policy. There is no permissive environment-variable approval switch.
`verified-peer-test-only` is an explicit API configuration for hermetic tests,
not a requirement or mode change for approved-phone qualification.

## Marker-scan ruling

The reference allowlist uses exact paths and SHA-256 digests. Exactly two
immutable upstream artifacts are separately excepted by exact path and digest:
`third_party/aasdk/src/Messenger/Cryptor.cpp` and
`third_party/aasdk/cert/headunit.key`. Task 2 pins all 566 vendor files byte for
byte. Any modification fails both provenance and the scan's digest exception;
these are not credentials consumed by the effective build. The locked TLS patch
removes the embedded literals, bundled credential files, and automatic install
behavior from the content-addressed effective stage. An effective-stage scan
has no upstream exceptions. Everything else with a private-key PEM marker is
rejected, including planted files and modified allowlisted references.

Upstream copyright and GPL-3.0-or-later terms apply; see THIRD_PARTY_NOTICES.md.
