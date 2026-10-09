# TLS and credential posture (host qualification)

## Identity and approval

AASDK is the TLS client. It presents the receiver/head-unit credential to the
phone and encrypts the session. This is not proof that the phone is approved.
Real phones are not assumed to supply verifiable peer certificates.

The real-phone policy is `encryption-only-compatibility`, emitted as `tls-mode=`
at Cryptor initialization. The upstream wrapper's unexamined `SSL_VERIFY_NONE`
default is replaced by `SSL_VERIFY_PEER`; compatibility relaxation exists only
in the explicit project policy branch. TLS 1.2 is the minimum. The Cryptor must
also have an approved-phone predicate, bound by its caller to the transport and
session identity. Absent, false or throwing predicates deny. Approval is checked
at initialization, each handshake step and before encrypted application I/O.
Deinitialization clears active state. TLS cannot substitute for the future task
27 approved-phone store and pairing policy, which are not implemented here.

`verified-peer-test-only` is an explicit constructor policy for tests, with
OpenSSL peer verification and an empty trust store by default. Unknown and
self-signed peer certificates fail closed. It is not a deployment default or
a prerequisite for the approved-phone qualification path. A trusted-peer
configuration/provisioning API is not claimed in this task.

## Explicit credentials

`HU_KEY_PATH` names one PEM bundle, certificate first and unencrypted private
key second. There is no implicit path, embedded fallback, bundled install, or
permission repair. The file must be regular, owned by the effective user, have
exact mode `0600` (no special permission bits), and have one hard link. The final
component cannot be a symlink. A single nonblocking, close-on-exec descriptor
is opened and checked with `fstat`, so permission checking does not reopen an
attacker-substituted pathname. Operators must control the parent directories;
parent-directory symlinks are not rejected. POSIX ACLs and concurrent in-place
modification by the same credential owner are not separately modeled.

Files are bounded to 64 KiB. OpenSSL parses certificate and key, encrypted-key
password prompts are disabled, the public/private components must match, and
non-whitespace trailing content is rejected. Typed errors contain only stable
failure classes; unset/empty credentials additionally explain HU_KEY_PATH.
No full path, phone identifier or PEM material enters the new diagnostics.
Parsed objects are RAII-owned until handed to Cryptor; its destructor releases
TLS resources, including after failed initialization.

The only committed credential is the existing public upstream compatibility
reference. See [its label and scan ruling](../third_party/compat-credentials/README.md).
It is not a production secret and is never installed or automatically loaded.
Provisioning production identity and validating it on a phone remain out of scope.

## Immutable vendor and locked effective patch

All 566 original AASDK files remain pinned by task-2 provenance. Staging first
verifies that gate and both patch digests. The existing GoogleTest patch remains
a unified diff. `deps/patches/aasdk-tls-credentials.patch` is a reviewed JSON
text-edit recipe (`aa-locked-source-patch/1`) rather than a unified diff: this
avoids retaining removed private-key PEM markers in diff artifacts. Each file
has a pinned original SHA-256; removals use unique delimiters, replacements must
match exactly once, and deletes are digest-anchored. `tools/deps/tls_patch.py`
applies it to memory, never to pristine sources. The effective stage identity
includes the TLS patch SHA-256 and every resulting file/mode digest. Reuse checks
all bytes, modes and paths. Effective Cryptor contains no embedded credential
symbols; bundled certificate/key files, install rules and Debian migration are
removed. Checked OpenSSL buffer-length conversions reject oversized lengths.

The repository marker scanner excludes only the root operational paths `.git`,
`.omo`, `build` and `.local`. All other content, including `__pycache__`
directories at every depth and source directories named `build`, is scanned.
Exact reference path/digest bindings and two separately labeled immutable upstream exceptions
implement the parent ruling. No broad vendor or compatibility-directory PEM
exemption exists. Effective-stage scanning has no exceptions.

## Executed host tests, not target acceptance

Run from the dedicated worktree:

```sh
cmake --preset host-dev -DAA_GOOGLETEST_ARCHIVE=/path/to/verified/googletest-v1.15.2.tar.gz
cmake --build --preset host-dev --parallel 4
ctest --preset host-dev -R tls_posture --output-on-failure
ctest --preset host-dev --output-on-failure
```

The memory-BIO harness uses the built, patched AASDK Cryptor, not a mock. Its
server pins the receiver reference certificate, requests client authentication,
checks the actual presented certificate and decrypts a test application record.
Compatibility succeeds only with the approved predicate. Unknown, absent and
revoked approval deny, as do inactive/deinitialized I/O. Verified-peer mode
rejects both the untrusted reference peer and a self-signed peer certificate
re-signed **in memory using the existing public reference key**; no new private
key is generated. Credential mismatch tests mutate only a temporary public
certificate modulus. Marker tests plant the existing reference PEM outside the
allowlist and mutate digest-pinned exceptions.

These tests establish host API/build behavior only. They do not qualify a real
phone, Jetson, deployment installation, pairing store, production credentials,
production TLS trust, cross compilation or target performance. No target or
phone contact, push, PR or acceptance-ledger update is part of this task.
