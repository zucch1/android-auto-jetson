# rtl8822ce-regd-shim — Jetson Wi-Fi regulatory fix

Registers the US regulatory domain on the RTL8822CE's self-managed wiphy — the call
NVIDIA's vendor driver never makes — and keeps it registered against the driver's
mode-transition clobbering. Tested on Jetson Orin Nano (kernel `6.8.12-1021-tegra`,
module `rtl8822ce` v5.14.0.4-261-g3e7be62b5 COEX).

## Root cause (evidence in the local evidence store, see below)

1. NVIDIA's `rtl8822ce` build compiles with `RTW_DEF_MODULE_REGULATORY_CERT=0x00`,
   so the FCC/CE/MIC country→plan tables do not exist in the module. The live
   country→plan map contains only `"00" → 0x7F` (world); `wlP1p1s0/country_code`
   reads `00` even with `rtw_country_code=US` set.
2. The driver marks the wiphy **self-managed** but never calls
   `regulatory_set_wiphy_regd*()`, so cfg80211's per-wiphy domain stays
   `country 00` with a crippled table (5 GHz capped at 20 dBm EIRP
   with 6 dBi antenna gain, NO-IR), which blocks AP beaconing on
   UNII-1/UNII-3.
3. A one-shot registration is not durable: the driver's AP/STA mode transitions
   re-assert the world domain (observed drift after qual runs).

## Components

| File | Purpose |
|---|---|
| `rtl8822ce-regd-shim.c` | GPL kernel module; registers the US regdomain on the wiphy at init (88 LOC) |
| `Makefile` | kbuild wrapper (`make -C /lib/modules/$(uname -r)/build M=$PWD`) |
| `rtl8822ce-regd-watchdog.sh` | drift convergence: re-asserts via `rmmod`+`modprobe` every 5 s |
| `rtl8822ce-regd-shim.service` | loads the shim at boot (waits for the netdev device unit) |
| `rtl8822ce-regd-watchdog.service` | runs the watchdog (`Restart=always`) |

## Install (on the target)

```bash
cd tools/jetson/rtl8822ce-regd-shim
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo install -D -m 0644 rtl8822ce-regd-shim.ko /lib/modules/$(uname -r)/extra/rtl8822ce-regd-shim.ko
sudo depmod -a
sudo install -m 0755 rtl8822ce-regd-watchdog.sh /usr/local/sbin/rtl8822ce-regd-watchdog
sudo install -m 0644 rtl8822ce-regd-shim.service rtl8822ce-regd-watchdog.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rtl8822ce-regd-shim.service rtl8822ce-regd-watchdog.service
```

The **device-ordered systemd unit is the sole boot loader** (`rtl8822ce-regd-shim.service`
waits for the `wlP1p1s0` device unit before modprobe). A `/etc/modules-load.d/` entry is
deliberately NOT used: it can run before the netdev exists and fail with `-ENODEV`,
producing an avoidable startup failure and relying on the service to recover.

## Verify

```bash
iw dev wlP1p1s0 info   # resolve the wiphy index for the target interface; do not assume phy#0
iw reg get             # in that phy's (self-managed) section, expect: country US: DFS-FCC,
                       # full rule table
iw phy                 # expect ch 36/44/48/149 without NO-IR; the dBm figures are the
                       # configured regulatory rule limits, not measured transmit power
lsmod | grep rtl8822ce_regd_shim
systemctl is-active rtl8822ce-regd-watchdog   # holds the domain after mode transitions
```

## Rollback

Note: **unloading the shim leaves the US domain applied** — module exit is
intentionally empty (cfg80211 keeps the previous wiphy regd private; there is
no restore entry point short of re-registering the wiphy). A **reboot** resets
the regd to the driver's own world default.

```bash
sudo systemctl disable --now rtl8822ce-regd-watchdog rtl8822ce-regd-shim
sudo rmmod rtl8822ce_regd_shim
sudo rm -f /etc/modules-load.d/rtl8822ce-regd-shim.conf \
           /lib/modules/$(uname -r)/extra/rtl8822ce-regd-shim.ko \
           /usr/local/sbin/rtl8822ce-regd-watchdog
sudo depmod -a
sudo reboot   # regd returns to the driver's default (world)
```

## Scope and honesty notes

- The shim registers the standard US rule set (as published in `regulatory.db`).
  Power stays the driver's conservative WW table — legal under FCC maxima; power
  maximization is deliberately not a goal.
- The shim does **not** change the driver's AP channel policy: the vendor driver
  forces a channel switch (CSA) off ch 36 to ch 44 for AP operation (documented in
  the evidence store; a source patch is the separate B-route option).
- Bluetooth (hci0/BlueZ) is untouched; coexistence behavior is unchanged by this module.

## Requalification after kernel or driver changes (issue #38)

The 2026-10-10/11 receipts (see Evidence below) were captured on one specific
stack: kernel `6.8.12-1021-tegra` with `rtl8822ce` v5.14.0.4-261-g3e7be62b5 COEX.
They do not transfer automatically. After any kernel, driver, shim, or watchdog
change, run this checklist on the device during an explicit maintenance window
(it runs nowhere else; none of it executes in CI or on the host):

1. **Record identity.** Capture `uname -r`, the `rtl8822ce` driver version
   (`modinfo rtl8822ce`), the shim source and built module (source SHA-256 plus
   `modinfo rtl8822ce-regd-shim.ko`), and the watchdog script hash. A
   requalification receipt names all four.
2. **Rebuild on-target.** Build against the running kernel's headers exactly as
   in the Install section; the existing `-Wall -Werror` flags must stay clean.
   A module built against different headers proves nothing about the new stack.
3. **Cold-boot ordering.** Reboot with both units enabled and save
   `journalctl -b -u rtl8822ce-regd-shim.service -u rtl8822ce-regd-watchdog.service`:
   the device-ordered unit must load the shim only after the netdev exists (no
   `-ENODEV`), and the watchdog must start and stay active.
4. **Verify the actual target wiphy.** Resolve the phy index from the interface
   (`iw dev <iface> info`; do not assume `phy#0`) and confirm that phy's
   `iw reg get` section reads `country US: DFS-FCC` with the full rule table
   after boot.
5. **Mode-transition drift and recovery.** Exercise AP/STA transitions and show
   via journal plus `iw reg get` that any drift back to `country 00` is
   re-asserted by the watchdog on its poll cycle.
6. **Bounded AP/client association.** Re-run the isolated AP and
   client-association check inside the existing qualified scope: ch 44 / 20 MHz
   AP, not ch 36 (the vendor driver forces a CSA 36 to 44). Keep the evidence
   pattern of the original qual runs (journal-valid cycles, `iw` captures).

Scope reminders for any requalification:

- Deployment intent is US-only. The shim registers the standard US rule set and
  power stays the driver's conservative table; RF maximization is not a goal.
- Historical receipts are evidence for the recorded stack only. After a kernel
  or driver change they are review inputs, not automatic qualification.
- The wireless gate status (`ap_startup_s` measured, `session_reconnect_s`
  unmeasured, claim 3 withdrawn) lives in
  [docs/capability-gate.md](../../../docs/capability-gate.md); this README does
  not change it. Existing qualification evidence is local-only (see Evidence
  below; the `.omo/` store is git-ignored and not part of the public tree).
- Host-side tests of the watchdog script run with mocked commands. They do not
  compile or load the out-of-tree module, touch hardware, or prove AP behavior
  or a full wireless session. Receiver host/ARM64 CI is not module qualification.

## License

Kernel module and supporting files: **GPL-2.0-or-later**, as marked by the
SPDX identifiers in each source file. This is a **component-scoped** license:
the repository's "GPL version 3" selection for the receiver components does
**not** apply to this kernel module. GPL licensing is the conventional choice
for Linux kernel modules; the regulatory setters used here are regular kernel
exports (`EXPORT_SYMBOL` in Linux v6.8).

## Evidence

Receipts, before/after captures, qual-run records, and the capability-gate drafts live
in the local evidence store:
`.omo/evidence/jetson-android-auto-receiver/task-12-fix/`
(`task-12-fix-receipt-2026-10-10.md`, `drift-addendum-2026-10-10.md`, `qual-run-*/`,
`qual-ht20-test/`, `qual-client-assoc/`, `claim-rescope-draft-2026-10-10.md`).
