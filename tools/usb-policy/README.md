# Scoped USB / ModemManager device policy (task 29)

Narrowly matched udev policy for the qualified Android Auto phone on the
qualified port of the qualified Jetson. It gives the unprivileged receiver
session user access to that one device's accessory bulk-pipe node and makes
ModemManager ignore exactly that one device (the task-11 finding: MM probes
the phone's CDC-ACM/`ttyACM` modem ports). Nothing else is matched, granted or
disabled.

```
tools/usb-policy/
  70-android-auto-receiver.rules   the policy (installed verbatim)
  install.sh                       bounded, idempotent installer
  rollback.sh                      restores the exact prior state (idempotent)
  rules_check.py                   stdlib-only fail-closed rules parser /
                                   structural matcher (also used by install.sh
                                   and tests/usb/test_usb_policy.py)
  README.md                        this file (ops + rollback doc)
```

## Scope: what matches

Exactly one physical port on the qualified Jetson, and exactly the qualified
identities on that port:

| Mode | idVendor:idProduct | Port |
| --- | --- | --- |
| Phone (MTP), Samsung Galaxy S24 Ultra | `04e8:6860` | `1-2.1` |
| AOA accessory | `18d1:2d00` | `1-2.1` |
| AOA + ADB accessory (post-switch enumeration) | `18d1:2d01` | `1-2.1` |

The same ids on **any other port do not match** - the port path is part of
every rule. No rule exists for any other vendor/product, port, hub, the
Bluetooth radio, HID dongles or the usb-serial class at large. There is no
world-readable mode, no blanket group access and no system-wide ModemManager
disable.

## Port-path encoding (kernel 6.8.12-1021-tegra, verified on target)

Verified with `udevadm info -a /sys/bus/usb/devices/1-2.1` on jetson.local:

- `KERNEL=="1-2.1"` / `KERNELS=="1-2.1"` - the device's kernel name **is** the
  bus-qualified stable topology path (bus 1 -> root port 2 -> hub port 1).
  `KERNEL` pins the device node; `KERNELS` pins the device or any descendant
  (interfaces, tty ports) of that one port.
- `ATTR{devpath}=="2.1"` - the hub-relative tail **without** the bus prefix.
  Deliberately not used: two buses on this target can both expose a `2.1`
  port, so `devpath` alone is not globally unique.

## Access control (device node, matched device only)

`GROUP="plugdev", MODE="0660", TAG+="uaccess"` on the `/dev/bus/usb/BBB/DDD`
node for the three identities above:

- `plugdev` + `MODE="0660"` is this project's established Android-accessory
  device-node convention (the same triple exists in the host-side ops rule
  `52-aa-probe.rules` and in the AASDK ops docs, which use `MODE="0666"` -
  **not** copied here: 0660 keeps "other" out). Group access is deterministic
  for any receiver process of a `plugdev` member, including SSH-started ones.
- `TAG+="uaccess"` additionally grants the **active seat session** user a
  POSIX ACL (logind revokes it when the session ends). Verified on target:
  the seat user of this device is the SSH/seat account, active on `seat0`.
- Least privilege: no `MODE="0666"`, no new group, no world access. The
  receiver user must be a `plugdev` member (one-time prerequisite:
  `sudo usermod -aG plugdev <user>`; `install.sh` warns when it is missing).

Note: the distro's broad `51-android.rules` (`adb_user` + vendor-wide
`04e8`/`18d1` matches) independently grants the same node access on this
image. That file is out of scope for this task and is not modified; these
rules stand alone on ids + port.

## ModemManager (matched device tree only)

`ENV{ID_MM_DEVICE_IGNORE}="1"` is set on **exactly the matched device tree**:
the usb device node, its interfaces and its tty ports (rule shape follows
ModemManager's own `77-mm-*-device-blacklist.rules`: id match via `ATTRS` on
the parent chain, plus the port constraint). Effect: MM never probes the
phone's CDC-ACM/`ttyACM` ports. Every other device stays visible to
ModemManager at default policy; the MM service is never stopped, masked or
globally disabled.

## Rule file numbering

`70-android-auto-receiver.rules` runs after `50-udev-default.rules` (so
`MODE`/`GROUP` for these exact nodes take effect) and before
`73-seat-late.rules` (so `TAG+="uaccess"` is registered before the `uaccess`
builtin applies the ACL) and before the `77-`/`80-` ModemManager rules. A
low `70-` prefix therefore applies our scoped additions without overriding
later system rules.

## Install

```sh
sudo -n bash tools/usb-policy/install.sh
```

Idempotent and fail-closed:

1. Validates the rules file first (`rules_check.py` strict parse and
   `udevadm verify` when available). Malformed input aborts **before any
   system state is touched**.
2. Records the exact prior state (file existence + `sha256` + a byte-exact
   backup of any pre-existing file) in
   `/var/lib/android-auto-receiver/usb-policy/`.
3. Installs the rules file to `/etc/udev/rules.d/70-android-auto-receiver.rules`
   (mode 0644) and reloads udev, then re-triggers **only** the qualified
   port's device tree (`udevadm trigger --action=change --parent-match=<syspath
   of 1-2.1>`; dry-run verified on target to select exactly that tree).
4. Re-running with no change is a no-op besides the reload; re-running after
   drift repairs the installed file while keeping the FIRST install's
   prior-state record authoritative for rollback.

Tests run the same scripts against a throwaway root via `AA_USB_POLICY_ROOT`
(no udevadm calls in that mode).

## Rollback

```sh
sudo -n bash tools/usb-policy/rollback.sh
```

Restores the **exact** prior state recorded by the first install:

- no file existed before -> the installed rules file is removed;
- a different file existed before -> its recorded bytes are restored
  (`sha256`-verified before and after);
- udev is reloaded and the qualified port's device tree is re-triggered, so no
  ACL or `ID_MM_DEVICE_IGNORE` state survives the removed rules (no stale
  ACLs);
- the install manifest and backup are removed - no trace of this tooling
  remains.

Idempotent: a second run with no recorded state is a clean no-op (exit 0).
Fail closed: an unknown/corrupt manifest or a hash-mismatched backup aborts
without deleting anything. Rollback is always the documented reversal of
`install.sh`; nothing else on the system is touched at any point.

## Tests and evidence

```sh
ctest --preset host-dev -R usb_policy --output-on-failure
```

Registers `usb_policy_rules` (structural match decisions, including the
negative same-ids-other-port case and malformed-input fail-closed),
`usb_policy_rollback` (install/rollback state machine in a fake root) and
`usb_policy_udevadm` (recorded `udevadm test` output parsing). The fixtures in
`tests/usb/fixtures/usb_policy/` freeze the target-side `udevadm test` runs
(serials redacted). The real target evidence round (install -> ACL proof ->
ModemManager ignore proof -> unmatched-device negative -> rollback proof) is
recorded in `.omo/evidence/jetson-android-auto-receiver/task-29-usb-policy.json`.
