# TLS credential configuration (downstream AASDK)

## No bundled or embedded credentials

This directory intentionally contains no `headunit.crt` or `headunit.key`.
The historical upstream certificate and private key were removed from this
distribution. AASDK's `Cryptor::init()` has no embedded-certificate fallback and
does not search `/etc/openauto`, `/usr/share/aasdk`, or relative `cert/` paths.

The old upstream instructions describing bundled credentials, automatic path
fallback, embedded PEM data, and world-readable private-key installation do not
apply to this tree.

## Required credential input

The downstream adapter calls `aa::tls::load_credentials()`. Set `HU_KEY_PATH` to
one PEM bundle containing, in order:

1. the head-unit certificate;
2. any intermediate CA certificates needed to build its chain; and
3. the matching private key (`PRIVATE KEY`, `RSA PRIVATE KEY`, or
   `EC PRIVATE KEY`).

The loader fails closed when the variable is unset, the file cannot be read,
the PEM structure is malformed, or the certificate and key do not match.
Encrypted/password-protected private keys are not accepted.

## File security requirements

`HU_KEY_PATH` must name a regular, non-symlink file owned by the receiver's
effective UID, with exactly mode `0600` and exactly one hard link. The bundle
must be non-empty and no larger than 65,536 bytes. Protect its parent
directories against replacement as part of service deployment.

Example for a service-specific account (substitute the actual account and
paths):

```bash
sudo install -o aa-receiver -g aa-receiver -m 0600 /secure/source/hu-bundle.pem \
  /var/lib/aa-receiver/tls/hu-bundle.pem
# The service environment must contain:
# HU_KEY_PATH=/var/lib/aa-receiver/tls/hu-bundle.pem
```

Do not install a private key with mode `0644`, and do not copy the removed
historical credential back into this repository. Credential provisioning and
rotation are deployment operations; this source tree neither generates nor
installs a usable head-unit credential.
