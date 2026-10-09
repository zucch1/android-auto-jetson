# android-auto-jetson

Public, GPL-3.0-or-later Android Auto receiver targeting NVIDIA Jetson.

**Status: repository bootstrap scaffold.** No implementation exists yet; the
files in this commit are governance and documentation skeletons only.

## Planned scope

- Standalone Android Auto receiver (wired USB/AOA first, receiver-hosted
  wireless second) for NVIDIA Jetson, isolated from any production
  infotainment application.
- Unprivileged receiver service for protocol/session/media plus a narrowly
  scoped privileged helper for NetworkManager AP, USB policy, ModemManager
  exclusion, and BlueZ pairing.
- Session D-Bus control/status and a bounded `AF_UNIX` `SOCK_SEQPACKET`
  channel for encoded H.264 to a consumer-side decode helper and dummy
  test display.
- Reproducible host and ARM64 cross-builds with pinned dependencies and a
  Jetson-derived sysroot.

## Governance

- Default branch: `main`. Protected: no force-push, no deletion.
- Pull requests merge **squash-only**; merge commits and rebase merges are
  disabled at repository level.
- **Zero required approvals** on `main` (checks gate, not reviewers).
- Branch naming convention: `aa/<topic>` for work branches applied on top
  of `main`.
- Author identity is repository-local:
  `Newton <59395246+zucch1@users.noreply.github.com>`.

## Documentation

- [ARCHITECTURE.md](ARCHITECTURE.md) - planned component layout (skeleton).
- [SECURITY.md](SECURITY.md) - vulnerability reporting and secrets policy.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE) for the full GNU General Public
License v3 text; project-owned code will carry "or any later version"
notices as it lands.
