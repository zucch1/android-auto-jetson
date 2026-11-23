# Security policy and threat model

## Supported versions and reporting

This project is in pre-release development. It has no Google Android Auto
certification or official SDK status. The implementation uses a
reverse-engineered protocol, so undocumented behavior, phone updates or
protocol changes can break compatibility and invalidate assumptions about
message handling. Report undisclosed vulnerabilities
privately through GitHub Security Advisories or contact the maintainer
(`@zucch1`) through GitHub. Acknowledgement is best effort within seven days.

## TLS and credentials

Live Android Auto projection uses TLS 1.2 with ECDHE-RSA AES-GCM by default.
The build option `AA_TLS_ALLOW_STATIC_RSA_COMPATIBILITY` adds static-RSA suites
for older peers. A connection that negotiates one of those suites loses forward
secrecy. Inbound AAP frames must be encrypted once TLS is active. The real-phone
compatibility policy uses TLS for encryption and relies on transport-bound
approval to admit the phone. Verified peer-certificate mode is available in
host tests. The generic TCP/TLS test adapter uses numeric IP endpoints and
defaults to a 100-second receive idle deadline.

The operator supplies `HU_KEY_PATH`, a certificate and matching unencrypted key
in a single regular PEM file owned by the receiver UID, with mode `0600` and
one hard link. The loader rejects a final-component symlink, bounds the read,
cleanses the temporary PEM buffer and rejects weak keys and signatures (112-bit
floor, plus rejection of SHA-1 and MD5 signatures). Expired HU certificates log
a warning and remain loadable per the owner's L6 decision. The operator
controls the parent directories and provisions and rotates the credential. The package
supplies no credential or fallback. See [TLS credential handling](docs/tls-credentials.md).

**Historical exposure (#89 M6).** The previously published public
compatibility HU credential was removed from the source tree and distribution,
but its private key remains in reachable Git history. Treat that credential as
exposed and replace it wherever it is still in use. The current tree has no
credential exception. The scanner can permit PEM armor for one labeled
public reference under `third_party/compat-credentials/` when both the exact
repository-relative path and SHA-256 match. The removed credential's digests
remain denied even if pinned as a reference. History remediation and cleanup
of any old local copies remain owner decisions before a release or tag. See the
[credential disposition](third_party/compat-credentials/README.md).

## Phone trust and local authorization

The approved-phone store contains raw identifiers. Its default location is
`$XDG_STATE_HOME/android-auto-receiver/approved-phones.json`, falling back to
`$HOME/.local/state/android-auto-receiver/approved-phones.json`. It uses an
owner-private directory and file, symlink-resistant directory traversal,
bounded schema validation and durable replacement. The trust store is
plaintext on disk. Store summaries use redacted labels.

Wired admission compares the approved USB descriptor identity. A serial-backed
approval can reconnect on another port. An explicit port-only approval is the
weaker tier for serial-less devices and covers indistinguishable devices
presenting that descriptor tuple on the same port. USB descriptors can be
spoofed by someone with physical access. Wireless enrolment binds a live
Bluetooth peer and adapter to explicit numeric-comparison consent. The helper
acknowledges the bond before the receiver persists approval. Subsequent
wireless projection also requires the approved-peer handoff and the AP
activation credential conveyed over the bonded Bluetooth link.

The receiver checks the session UID and registered consumer's D-Bus identity
for consumer-authorized operations. Processes running as the trusted session
UID are inside the security boundary. The production helper uses the fixed
root-owned system bus socket and the client requires a root-owned helper.
Unix bus connections check the socket owner before authenticating and the
built-in bus adapter checks the GUID when pinned in the address. The session
bus still comes from the receiver's environment. If that environment is in
scope for a deployment, pin the trusted session bus address and GUID.

The helper's packaged D-Bus policy denies calls by default. The operator
provisions the session UID, receiver executable digest and a per-UID delivery
rule. For each request the helper resolves the unique caller and verifies the
UID, PID, process start time and executable digest. Privileged effects are
limited to the supported operations. Scoped grants expire, and the helper
rejects reused grants during its process lifetime. The helper uses wiped buffers for the copies of the AP passphrase
it controls and disables GDBus message debugging. GLib serialization, kernel
socket buffers, NetworkManager and an authorized debugger can still have access
to runtime secrets. See [system-helper authorization](docs/system-helper.md).

## Untrusted inputs and diagnostics

Treat phone bytes (USB, Bluetooth and Wi-Fi) and IPC frames as untrusted,
including after device approval. Framing, protocol parsing, IPC dispatch and
persistent-state parsing validate lengths, types and bounds before processing.
Media and input use local sockets with peer checks. Admission limits and
deadlines mitigate resource exhaustion. Compromised approved peers may still
cause service degradation.

Structured diagnostics use field-level redaction and per-process pseudonyms
for identifiers. These labels use noncryptographic hashes for in-run
correlation. Do not publish raw logs, captures,
trust/helper state, AP credentials, Bluetooth link keys or the HU PEM.
Media telemetry has bounded queues and rate limits. The receiver does not
manage cumulative log retention. Configure and verify size and retention caps
in the deployment's journal or rotating log sink (#89 L13).

## Updates and removal

The Debian package ships a default-deny helper policy and service files.
Activation and credential provisioning are operator steps. Both packaged
systemd units set `LimitCORE=0`. Preserve that limit in custom units and
disable core dumps for manual receiver launches. Runtime and deployed
collector policy determine actual core retention.

After an upgrade or rollback, refresh the helper's configured digest from the
installed receiver and restart the affected services using the
[deployment procedure](docs/deploy.md). Package removal attempts to stop the
running helper before removing policy and reloads udev. Stop and disable the
user receiver before removal. Operator-owned credentials, phone approvals,
helper state, local policy overrides, retained deployment artifacts, logs and
OS Bluetooth pairings require separate cleanup. See the
[Debian package guide](docs/debian-package.md).

`ForgetPhone` revokes local projection approval. Wireless forget also requests
AP credential rotation and demotes the bond to refused. The BlueZ pairing
remains. If helper cleanup is incomplete, the call can return an error after
local revocation, and the consumer can retry the same phone ID. Use the
Bluetooth UI to unpair if the bond itself should be removed. See
[ForgetPhone semantics](docs/INTEGRATION-CONTRACT.md#forgetphone-partial-outcomes-and-approved-phone-listing).

## Verification and limits

From a configured native `host-dev` build:

```sh
cmake --build --preset host-dev --target security_hardening_tests
ctest --preset host-dev --no-tests=error -R security_hardening --output-on-failure
python3 -B tools/tls/scan.py .
```

CTest also selects the same profile with `-L '^security_hardening$'`. The
fixtures cover authorized D-Bus, TLS and wired admission flows. They assert
that unknown local peers and phones, malformed protobuf and insecure HU key
permissions fail closed. The profile also checks seeded framing fuzz smoke,
helper authorization, trust-store permissions, diagnostic redaction and
credential scanning, including the exact-path and digest rule.

The source-tree scanner recognizes key and certificate PEM armor, selected
GitHub token formats, historical HU filenames and pinned historical HU
digests, including supported re-encodings. It rejects symlinks, special files
and unreadable entries outside its top-level local metadata and build
exclusions. Coverage excludes Git history, arbitrary binary containers and
unrecognized secret formats.

The host profile uses disposable fixtures. Installed-system hardening and
storage retention remain deployment checks. Observed-sysroot ELF/runtime
qualification and repository-side CI workflow trust enforcement are tracked
as open items in [the security audit](https://github.com/zucch1/android-auto-jetson/issues/89).
