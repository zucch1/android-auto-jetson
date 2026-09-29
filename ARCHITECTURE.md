# Architecture

**Status: planned skeleton. Nothing below is implemented yet.** This file
records the intended component boundaries so later work follows them.

## Components (planned)

- `aa-receiver.service` (unprivileged user session): Android Auto protocol
  and session state, media handling, D-Bus control/status surface.
- `aa-system-helper.service` (privileged, narrowly scoped): NetworkManager
  access point management, USB policy, ModemManager exclusion, BlueZ
  profile registration and pairing mediation.
- Consumer-side GStreamer decode helper and dummy test display.
- Bounded `AF_UNIX` `SOCK_SEQPACKET` channel carrying encoded H.264 access
  units (timestamps, sequence numbers, IDR flags, drop-to-IDR recovery).

## Transport order (planned)

1. Wired USB / Android Open Accessory.
2. Receiver-hosted wireless via a dedicated fixed-channel 5 GHz access
   point (explicit jurisdiction handling; fail-closed channel validation).

## Boundaries (planned)

- This repository owns the receiver sidecar and its test consumer only; it
  does not own any production infotainment UI, autostart, or display
  activation.
- Low-rate control/status over session D-Bus; bulk media over the AF_UNIX
  socket; no high-rate audio/video over D-Bus.
- Dependencies are pinned and vendored with provenance and license review
  (GPL-3.0-or-later compatible).