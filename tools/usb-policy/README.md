# Scoped USB / ModemManager device policy (task 29, #133)

Narrowly matched udev policy for the approved Android Auto phone ids on any
host USB port of the qualified Jetson. It gives the unprivileged receiver
session user access to the matched device's accessory bulk-pipe node and makes
ModemManager ignore exactly the matched device (the task-11 finding: MM probes
the phone's CDC-ACM/`ttyACM` modem ports). Nothing else is matched, granted or
disabled.

```
tools/usb-policy/
  70-android-auto-receiver.rules   the policy (installed verbatim)
  install.sh                       bounded, idempotent installer
  rollback.sh                      restores the exact prior state (idempotent)
  common.sh                        shared trust/staging/state-validation
                                   helpers sourced by both scripts
  rules_check.py                   stdlib-only fail-closed rules parser /
                                   structural matcher + exact-policy gate
                                   (also used by install.sh and
                                   tests/usb/test_usb_policy.py)
  README.md                        this file (ops + rollback doc)
```

## Scope: what matches

Exactly the approved identities, on **any** host USB port:

| Mode | idVendor:idProduct |
| --- | --- |
| Phone (MTP), Samsung Galaxy S24 Ultra | `04e8:6860` |
| AOA accessory | `18d1:2d00` |
| AOA + ADB accessory (post-switch enumeration) | `18d1:2d01` |

No rule exists for any other vendor/product, hub, the Bluetooth radio, HID
dongles or the usb-serial class at large. There is no world-readable mode, no
blanket group access and no system-wide ModemManager disable.

## No port constraint (#133)

Until #133 every rule also pinned `KERNEL(S)=="1-2.1"`. The owner decided on
2026-11-15 to drop the pin: USB ids are spoofable, so a spoofing device could
use the pinned port as easily as any other, and the pin only stranded the
approved phone when it was plugged into another hub socket. Device-node access
is not trust. The receiver's approved-phones store decides admission:
serial-backed wired identities match on any port, and a port-only
(serial-less) approval still matches only its own port (see
`include/aa/trust/Identity.hpp`).

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
rules stand alone on ids.

## ModemManager (matched device tree only)

`ENV{ID_MM_DEVICE_IGNORE}="1"` is set on **exactly the matched device tree**:
the usb device node, its interfaces and its tty ports (rule shape follows
ModemManager's own `77-mm-*-device-blacklist.rules`: id match via `ATTRS` on
the parent chain). Effect: MM never probes the
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

1. Captures the rules file into a private snapshot and validates **that one
   copy** (`rules_check.py` verify - which requires byte identity with the shipped policy,
   not merely parseable rules - and `udevadm verify` when available); the same
   snapshot is hashed and installed, so a source swap between steps can
   neither install rejected content nor record a wrong digest. Malformed or
   non-approved input aborts **before any system state is touched**.
2. Verifies the destination/state ancestry (real, caller-owned,
   non-group/other-writable directories; no symlinked or irregular
   destination, manifest or backup entries) and records the exact prior state
   (file existence + `sha256` + a byte-exact backup of any pre-existing file)
   in `/var/lib/android-auto-receiver/usb-policy/`.
3. Installs the rules file to `/etc/udev/rules.d/70-android-auto-receiver.rules`
   (mode 0644) via staged temp + atomic rename - the destination is never
   reopened for writing or chmod - and reloads udev, then re-triggers **only**
   the device trees of attached devices carrying an approved id, on whatever
   port they are on (`udevadm trigger --action=change --parent-match=<syspath>`
   per device; the id list is `APPROVED_USB_IDS` in `common.sh`, which a test
   keeps equal to the rules file).
4. Re-running with no change is a no-op besides the reload; re-running after
   drift repairs the installed file while keeping the FIRST install's
   prior-state record authoritative for rollback. `install.sh` and
   `rollback.sh` serialize on an exclusive `flock` of the stable `/var/lib` ancestor and
   record the transaction phase before every destructive change, so an
   interrupted run is recovered instead of re-basing the backup.

The initial backup and manifest are staged privately and published together
by directory rename, before the policy changes. Rollback retains that pair
through restoration and udev reconciliation, then retires the directory
atomically before disposal. Fault-injection tests cover every recorded phase,
destination publication, reload failure, and state retirement with both
present and absent baselines. An untrappable process kill can leave private
staging/retirement directories; these never become active rollback records.
This is process-interruption recovery, not a qualified power-loss guarantee.

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
- udev is reloaded and every attached approved-id device tree is re-triggered:
  rule-derived udev properties - `ID_MM_DEVICE_IGNORE` above all - are
  re-evaluated and disappear with the removed rules (verified on target, see
  the evidence receipt);
- the install manifest and backup are removed - no trace of this tooling
  remains.

**Named limitation - named-user ACLs are NOT revoked by rollback.** The
`uaccess` builtin applies its POSIX ACL only while a rule tags the device at
event-processing time; removing this rule set does **not** flush an ACL that
is already on the node, and a successful reload+trigger is **not** evidence
that one was revoked. Such an ACL persists until the seat session ends
(logind revokes the grants it applied), the device re-enumerates, or it is
removed explicitly (`setfacl -b /dev/bus/usb/BBB/DDD`). On this image the
distro's `51-android.rules` additionally tags the qualified vendor ids
`uaccess`, so an equivalent access grant can legitimately survive this
rollback - that grant is independently authorized and is deliberately left
untouched (this tooling cannot attribute an ACL entry to a single rule).
Rollback therefore restores rules state and re-triggers udev only; it makes
no live ACL-revocation claim.

Idempotent: a second run with no recorded state is a clean no-op (exit 0).
Fail closed: an unknown/corrupt manifest, a hash-mismatched backup or an
inconsistent saved state aborts without deleting anything. Interrupted
install/rollback runs are recovered from the recorded transaction phase
(`installing`/`installed`/`restoring`/`restored`): a rerun of `rollback.sh`
finishes an interrupted rollback instead of refusing, and the third-party
drift refusal is unchanged. Rollback is always the documented reversal of
`install.sh`; nothing else on the system is touched at any point.

## Tests and evidence

```sh
ctest --preset host-dev -R usb_policy --output-on-failure
```

Registers `usb_policy_rules` (structural match decisions, including the
approved ids on every port, unapproved near-miss ids on every port, no port
pin, and malformed-input fail-closed),
`usb_policy_rollback` (install/rollback state machine in a fake root) and
`usb_policy_udevadm` (recorded `udevadm test` output parsing). The fixtures in
`tests/usb/fixtures/usb_policy/` freeze the target-side `udevadm test` runs
(serials redacted). The real target evidence round (install -> ACL proof ->
ModemManager ignore proof -> unmatched-device negative -> rollback proof) is
recorded in `.omo/evidence/jetson-android-auto-receiver/task-29-usb-policy.json`.
