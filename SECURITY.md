# Security Policy

## Supported versions

None yet. This repository is at bootstrap stage and has no releases.

## Reporting a vulnerability

Report privately via GitHub Security Advisories on
`zucch1/android-auto-receiver`, or contact the maintainer through GitHub
(`@zucch1`) with the `zucch1` account's public noreply address. Do not open
public issues for undisclosed vulnerabilities.

## Secrets policy

- Never commit secrets: no tokens, no production private keys, no phone
  identifiers or captures, no SSH material.
- Protocol credentials shipped for compatibility (for example a public
  reference head-unit credential) must be labeled public and accompanied by
  documentation of their exposure and scope.
- Local planning, evidence, and sysroot content (`.omo/`, `.local/`) stays
  untracked by design; see `.gitignore`.

## Reporting turnaround

Acknowledgement best effort within 7 days; this is a single-maintainer
pre-release project and makes no SLA commitment yet.