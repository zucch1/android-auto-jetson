# USB / AOA capability probe (task 11)

`tools/target/probe-usb-aoa.sh` is a bounded, reusable wired-USB/Android Open
Accessory capability probe for one explicitly selected phone on the Jetson.
It is host code that streams its target modules over SSH stdin; it does not
write a file on the target. Tests are entirely hardware-free, so a green test
run proves the boundary logic, not the device: **an owner physically connected
approved phone connected to a Jetson host-mode hub port is required to qualify
phone hardware.** A separate Jetson device-mode USB-C data cable to the host is
required to observe active USB-C gadget coexistence.

## Usage

```sh
tools/target/probe-usb-aoa.sh --help
tools/target/probe-usb-aoa.sh --jetson jetson.local \
    --port 1-2.1 --phone-id 04e8:6860 --duration 30
```

- `--port` is the kernel's stable physical path (sysfs name such as `1-2.1`).
- `--phone-id` is the exact approved `vendor:product` (the known phone is
  `04e8:6860`). Passing **both** is the owner's authorization to issue the
  bounded AOA control/switch requests for that one port; there is no separate
  `--approved-phone` flag because the port plus known id already qualify the
  selector. Any other device on that port is reported as an unqualified
  candidate and receives **zero** control requests.
- `--cycles N` (0..10) explicitly exercises `N` targeted software resets. The
  reset is `libusb_reset_device` on the selected phone only; it is labelled a
  *software port reset*, never a physical power-off. No hub or whole-controller
  reset is performed.
- `--duration S` (0..120, default 30) samples stable USB presence and the
  advertised `bMaxPower` budget. The advertised descriptor value is recorded as
  an advertisement, **not** a measured current.
- `--sudo` streams through `sudo -n python3 -` on the target when opening the
  device needs privilege. No privileged config service is changed.

SSH always uses `BatchMode=yes`, `StrictHostKeyChecking=yes`,
`HostKeyAlgorithms=ssh-ed25519`, `ControlMaster=no`, `ControlPath=none` and
`ConnectTimeout=10`. `--source-address` and `--address` select the local bind
and destination addresses and `--host-key-alias` sets `HostKeyAlias`; no host
interface assignment is hardcoded. Sources stream over stdin, so there is no
target staging directory and no leftover runtime file.

## What is reported

Stdout is exactly one JSON object (`aa-usb-aoa-probe/1`); diagnostics go to
stderr and the exit status is nonzero for a real failure or an unqualified
candidate. The report covers:

- Device selection and sysfs descriptor/serial/endpoint evidence, including the
  post-switch bulk IN/OUT addresses and `wMaxPacketSize` (512 bytes observed).
- The AOA handshake: `GET_PROTOCOL` must return exactly 2 bytes and a version
  `>= 1`; every `SEND_STRING` must return the full NUL-terminated length; `START`
  must return 0. A short read, short write or transfer error stops the sequence
  **before** `START`. No bulk/protocol traffic is sent and no Android Auto
  session is claimed.
- The pinned accessory strings are the checked-in AASDK reference values
  (`third_party/aasdk/src/USB/AccessoryModeQueryFactory.cpp`): manufacturer
  `Android`, model `Android Auto`, description `Android Auto`, version `2.0.1`,
  URI `https://f1xstudio.com`, serial `HU-AAAAAA001`.
- A device already in AOA mode (`18d1:2d00` / `18d1:2d01`) on the same port is
  accepted without re-running the switch only when usable bulk IN/OUT endpoints
  and stable AOA presence are observed. Successful START alone does not qualify
  a phone: same-port AOA re-enumeration must also be observed within 15 seconds.
- Optional software-cycle outcomes: bounded same-port re-enumeration
  (15 s) and AOA re-establishment if the phone returns as MTP.
- Gadget snapshot from `/sys/class/udc` and configfs. `active` requires a gadget
  actually bound to a UDC reporting `configured`; a merely configured-but-idle
  controller is reported as idle coexistence and never as an active pass.
- Presence-only host evidence: ModemManager `systemctl is-active`, a bounded
  30-second `journalctl` window, `mmcli` availability, and `adb` availability
  from `shutil.which`. `adb` is never invoked and no package is installed.

## Protected-path evidence

`protected_inventory` is `external-proof-pending` by default — never a false
pass. The parent wraps a hardware run with the existing
`tools.target_safety` snapshot producer and can pass `--before-manifest` /
`--after-manifest`; the probe then validates them with the existing
`snapshot_diff.diff_manifests` consumer and records the verdict, adding a blocker
on any non-equivalent result. The probe does not create inventories itself.

## Regressions

```sh
python3 -B tests/usb/test_probe.py            # hardware-free regressions
python3 -B tests/usb/assert_coverage.py build/host-dev
```

The suite exercises the real boundary functions: an unknown device issues no
requests, a short `GET_PROTOCOL` is rejected, a short `SEND_STRING` never reaches
`START`, a wrong port does not pass, the exact requests/lengths are pinned,
transfer errors clean up the handle, and already-AOA plus software-cycle
re-enumeration qualify. `cmake/UsbAoaProbe.cmake` registers `usb_aoa_selection`,
`usb_aoa_handshake`, `usb_aoa_libusb`, `usb_aoa_sysfs`, `usb_aoa_hostenv`,
`usb_aoa_cycles`, `usb_aoa_cli` and `usb_aoa_coverage`.

## Honest completion criteria

Host tests and registration do not qualify hardware. Actual device capability,
software-reset behavior and active gadget coexistence require a live CLI run
with the approved phone on the host-mode hub and the separate gadget cable
connected. Hardware capability and the protected-inventory verdict are separate
results: missing or failed safety evidence cannot close task acceptance. Owner
physical actions are observations, not agent-automated acceptance passes.
