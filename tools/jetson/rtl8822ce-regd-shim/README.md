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
   `country 00` with a crippled table (5 GHz at 6 dBm / NO-IR), which blocks
   AP beaconing on UNII-1/UNII-3.
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
make -C /lib/modules/$(uname -r)/build M=$PWD modules
sudo install -m 0644 rtl8822ce-regd-shim.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
echo rtl8822ce_regd_shim | sudo tee /etc/modules-load.d/rtl8822ce-regd-shim.conf
sudo install -m 0755 rtl8822ce-regd-watchdog.sh /usr/local/sbin/rtl8822ce-regd-watchdog
sudo install -m 0644 rtl8822ce-regd-shim.service rtl8822ce-regd-watchdog.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now rtl8822ce-regd-shim.service rtl8822ce-regd-watchdog.service
```

## Verify

```bash
iw reg get    # expect: phy#0 (self-managed) country US: DFS-FCC with the full rule table
iw phy        # expect: ch 36/44/48/149 without NO-IR; 5 GHz power up to 23-30 dBm
lsmod | grep rtl8822ce_regd_shim
systemctl is-active rtl8822ce-regd-watchdog   # holds the domain after mode transitions
```

## Rollback

```bash
sudo systemctl disable --now rtl8822ce-regd-watchdog rtl8822ce-regd-shim
sudo rmmod rtl8822ce_regd_shim
sudo rm -f /etc/modules-load.d/rtl8822ce-regd-shim.conf \
           /lib/modules/$(uname -r)/extra/rtl8822ce-regd-shim.ko \
           /usr/local/sbin/rtl8822ce-regd-watchdog
sudo depmod -a
```

## Scope and honesty notes

- The shim registers the standard US rule set (as published in `regulatory.db`).
  Power stays the driver's conservative WW table — legal under FCC maxima; power
  maximization is deliberately not a goal.
- The shim does **not** change the driver's AP channel policy: the vendor driver
  forces a channel switch (CSA) off ch 36 to ch 44 for AP operation (documented in
  the evidence store; a source patch is the separate B-route option).
- Bluetooth (hci0/BlueZ) is untouched; coexistence behavior is unchanged by this module.

## License

Kernel module: **GPL-2.0-or-later** (component-scoped — it uses GPL-only exports
`regulatory_set_wiphy_regd_sync`/`regulatory_set_wiphy_regd`). This component's
license does not extend to the repository's other components.

## Evidence

Receipts, before/after captures, qual-run records, and the capability-gate drafts live
in the local evidence store:
`.omo/evidence/jetson-android-auto-receiver/task-12-fix/`
(`task-12-fix-receipt-2026-10-10.md`, `drift-addendum-2026-10-10.md`, `qual-run-*/`,
`qual-ht20-test/`, `qual-client-assoc/`, `claim-rescope-draft-2026-10-10.md`).
