# Jetson RTL8822CE rebuild (issue #73)

Target: Orin Nano Super. Originally L4T R39.2.1 (kernel `6.8.12-1021-tegra`); rebuilt for R39.2.2 (kernel `6.8.12-1029-tegra`) on 2026-11-21, see [R39.2.2 rebuild](#r3922-rebuild-2026-11-21).
Operate only over `ssh zucchi@192.168.55.3` (Ethernet); Wi-Fi is the only
internet uplink. Download all inputs before unloading the module.

## Sources and build

Use NVIDIA's [JetPack 7.2.1 downloads](https://developer.nvidia.com/embedded/jetpack/downloads)
→ Driver Package (BSP) Sources:

```
https://developer.nvidia.com/downloads/embedded/L4T/r39_Release_v2.1/sources/public_sources.tbz2
https://developer.nvidia.com/downloads/embedded/L4T/r39_Release_v2.1/sources/public_sources_sha.txt
```

The downloaded outer archive SHA-256 is
`8346f0371649a92c42293b2d4a7bf3d69e4dd1de23009e1463ea910994a81371`.
The nested `kernel_oot_modules_src.tbz2` SHA-1 matches NVIDIA's published list:
`f2d68964b2de81504bcd47f0fc905ad287c7d305`. Its bundled checksum uses an
absolute NVIDIA build-machine path; compare the digest against the local file.
The driver is `nvidia-oot/drivers/net/wireless/realtek/rtl8822ce`, version
`v5.14.0.4-261-g3e7be62b5.20250204_COEX20240327-2727`, exactly matching the
original installed module. No third-party fork is used.

Extract outside the repository into `~/rtl8822ce-r39.2.1/source`, then build
on the Jetson:

```sh
cd ~/rtl8822ce-r39.2.1/source/nvidia-oot/drivers/net/wireless/realtek/rtl8822ce
make -j4 ARCH=arm64 \
  srctree.nvidia-oot="$HOME/rtl8822ce-r39.2.1/source/nvidia-oot" \
  USER_EXTRA_CFLAGS='-DCONFIG_REGD_SRC_FROM_OS -DCONFIG_FORCE_SW_CHANNEL_PLAN'
modinfo -F vermagic ./rtl8822ce.ko
modinfo -F vermagic rtl8822ce
```

Both flags are necessary: `hal_com_config_channel_plan()` otherwise honors
the efuse force bit, and `rtw_regd_is_wiphy_self_managed()` then keeps the radio
self-managed even with OS regulatory support compiled. OS support defaults
`CONFIG_RTW_REGD_SRC` to 1. No driver source edits are needed. The first build
with only OS support was never loaded. The installed headers are accessible
through `/lib/modules/$(uname -r)/build`. GCC 13.3 emits a compiler-version
warning against the kernel's GCC 13.2.

## Backup and detached trial

Before installation, preserve both `~/rtl8822ce-backup/rtl8822ce.ko` and
`rtl8822ce.ko.orig` beside the installed module. Original MD5:
`e332b02106c13446140ea7ea9ee70c69`; SHA-256:
`a73277dc07b1b0395e01c4691153ed20318fe295de30e5dad53c994a32159ea5`.
Never replace either backup with a candidate.

Copy `tools/jetson/rtl8822ce-trial.sh` to the target and run through
`sudo systemd-run --unit=aa-wifi-trial /bin/bash SCRIPT CANDIDATE ORIGINAL os LOG`.
All paths must be absolute. The script loads from the build path without
installing, stops the shim/watchdog for the OS-managed candidate, keeps US and
existing power-management options, and excludes DFS channels 52–144.
Success requires Renzo on `wlP1p1s0` and a ping bound to that interface within
60 seconds. Failure restores the original and its shim/watchdog, restarting
supplicant and NetworkManager; a second failure reboots. A transient 170-second
deadman timer also reboots if the script stalls. The installed module must
therefore already be known good. Inspect the script log over Ethernet.

## Installation and rollback

Only install after the trial and regulatory checks pass. Preserve
`rtw_country_code=US` and existing options. OS mode requires the old self-managed
shim/watchdog to remain stopped; record their prior enablement for rollback.
Exclude `52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144` through
`rtw_excl_chs` so DFS cannot be used for AP operation.

Persist exclusions in `/etc/modprobe.d/rtl8822ce-issue73.conf` with one line:

```text
options rtl8822ce rtw_excl_chs=52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144
```

Disable `rtl8822ce-regd-shim.service` and `rtl8822ce-regd-watchdog.service`
for OS mode; both were enabled before this rebuild. Leave `wifi-regdom.service`
enabled to set the global US domain at boot.

The install path depends on the kernel. Set `K` to the target kernel. After a reboot this is the
running kernel, `K=$(uname -r)`, which is `6.8.12-1029-tegra` since the R39.2.2 upgrade:
`/lib/modules/$K/updates/drivers/net/wireless/realtek/rtl8822ce/rtl8822ce.ko`.
Install the verified candidate there with mode 0644, then run `sudo depmod -a $K`.
The original R39.2.1 install used `K=6.8.12-1021-tegra`. That tree is historical: the upgrade
removed its module, so don't install to it or roll back into it.
To roll back on kernel `$K`, restore `rtl8822ce.ko.orig` from the same `$K` directory (the stock
module for that kernel), then run `sudo depmod -a $K`. Remove only
`/etc/modprobe.d/rtl8822ce-issue73.conf` and leave the original `rtl8822ce.conf` untouched. Re-enable
both shim/watchdog units, but only once a shim built for `$K` exists under `/lib/modules/$K/extra/`
(see [R39.2.2 rebuild](#r3922-rebuild-2026-11-21)). Then run the detached trial script with the
backup as both candidate and original, mode `vendor`.
Check Renzo and interface-bound ping. Returning to the new driver uses mode `os`
and reinstalls the verified candidate and its DFS exclusions.

## Verification record

2026-11-14/15, direct Ethernet management:

- Native build passed; candidate SHA-256
  `6fef3ad89fa854b87806db665f93594d61f5857a40638e6101545aaedee4cee0`.
  Vermagic exactly matches `6.8.12-1021-tegra SMP preempt mod_unload modversions aarch64`.
- Detached trial passed in six seconds; Renzo and interface-bound internet ping
  recovered. Driver reports `regd_src:OS(1)`, `alpha2:US`, `chplan:NA`.
  `NA` is expected for OS mode and replaces the Realtek `0x7F` plan.
- Channels 36–48 have no NO_IR. DFS 52–144 are excluded from vendor chset;
  cfg80211 still marks them radar-required. No DFS AP was started.
- Temporary NetworkManager AP held 36/5180 MHz for more than five minutes,
  checked every five seconds, including three `aa-system-helper` restarts.
  Renzo was disconnected throughout the hold. Cached scan entries contained
  neighbours on 44/48/157/161, none on 36. The temporary AP profile was deleted
  and Renzo/internet restored afterwards.
- Full rollback passed: restored original installed file and checksum, ran
  depmod, restored original shim/watchdog state and reloaded the original driver.
  World-plan behavior returned and Renzo/internet recovered in 12 seconds.
  Returned to the verified new module and restored its persistent configuration.
- One reboot passed: installed candidate, OS/US domain, channel eligibility,
  DFS exclusions, Renzo/internet, and helper service all recovered.
  A second five-minute channel-36 AP hold and three further helper restarts
  also passed after reboot. Cleanup removed the test profile; Renzo internet
  ping passed again. No test AP or recovery timer remains.

This AP test follows the existing target `~/aa-build/task38_chtest.sh` NM
profile path, extended with a five-minute hold and helper restarts. It does
**not** prove authenticated receiver/helper-owned AP reacquisition. The
production `ap.conf` remains channel 44 and was not modified; a receiver test
must use matching field 6 and `--ap-channel 36`. Trust stores, Bluetooth bonds,
and `/usr/libexec/aa-system-helper` were not modified. RF-isolated verification
was not performed. Keep issue #73 open for the remaining qualification.

Full operator log: `/home/zucchi/aa-research/issue73-rebuild-log.md` on the laptop;
raw trial/build/AP logs are outside the repository.

## R39.2.2 rebuild (2026-11-21)

**Every L4T BSP update reverts this.** `nvidia-l4t-kernel-oot-modules` owns
`/lib/modules/<kver>/updates/drivers/net/wireless/realtek/rtl8822ce/rtl8822ce.ko`.
An upgrade installs NVIDIA's stock driver for the new kernel. On the next boot the radio is self-managed with the
world plan again (`iw reg get` shows `phy#0 (self-managed)` / `country 00`), and
channels 36–48 are NO_IR for AP use. Station mode, including the Renzo uplink, keeps working on the
stock driver. Before any `apt full-upgrade` that touches `nvidia-l4t-*`, plan this rebuild.

What changed in R39.2.2:
- The kernel is `6.8.12-1029-tegra`, so the module directory changed. The upgrade removed the 1021
  module and left only `rtl8822ce.ko.orig` in the 1021 tree.
- Sources: `https://developer.nvidia.com/downloads/embedded/L4T/r39_Release_v2.2/sources/public_sources.tbz2`
  (outer SHA-256 `5cab0eff33610f0605ebda3e7d9c5deb950a538556cb4cdf198021d48f9897ef`). The nested
  `Linux_for_Tegra/source/kernel_oot_modules_src.tbz2` SHA-1 `a9da605a0fba96da8445b481dacd3859215a3545`
  matches NVIDIA's `public_sources_sha.txt`.
- The driver version string is unchanged (`v5.14.0.4-261-g3e7be62b5.20250204_COEX20240327-2727`),
  but NVIDIA changed about 20 source files. Both `CONFIG_REGD_SRC_FROM_OS` and
  `CONFIG_FORCE_SW_CHANNEL_PLAN` paths are still present, and the regulatory/channel-plan code is unchanged.
- The driver Makefile gains `CONFIG_RTW_DBG_PROC = n` (and `ifeq ($(CONFIG_RTW_DBG_PROC), y)
  EXTRA_CFLAGS += -DCONFIG_RTW_DBG_PROC`), so `/proc/net/rtl88x2ce/<if>/chan_plan` no longer exists.
  Verify manually with `iw reg get` (no `self-managed` phy) and `iw phy` (36–48 without `no IR`, 52+
  `radar detection`). `rtl8822ce-trial.sh` still `cat`s `chan_plan` after `TRIAL_OK`, so a successful
  trial log now ends with a harmless `No such file or directory`.
- The regulatory shim (`tools/jetson/rtl8822ce-regd-shim`) is installed per kernel under
  `/lib/modules/<kver>/extra/`, so the upgrade left none for 1029. The trial's failure path and the vendor
  rollback restart `rtl8822ce-regd-shim.service`. Without a shim for the running kernel they fail and
  reboot instead of recovering. **Rebuild and install the shim for the new kernel before running the
  trial** (see its README; install only, and keep the units disabled in OS mode). Done for 1029 on
  2026-11-21.

### Procedure (as run 2026-11-21)

1. **Before upgrading**, while the uplink is on the old driver:
   - back up the installed and `.orig` modules to `~/rtl8822ce-backup/`;
   - download and verify the new sources;
   - extract the driver:

   ```sh
   mkdir -p ~/rtl8822ce-r39.2.2 && cd ~/rtl8822ce-r39.2.2
   # download public_sources.tbz2 + public_sources_sha.txt, check digests, then:
   tar -xjf public_sources.tbz2 --wildcards '*kernel_oot_modules_src.tbz2'
   mkdir -p source && tar -xjf Linux_for_Tegra/source/kernel_oot_modules_src.tbz2 -C source --wildcards 'nvidia-oot/*'
   ```

2. **Upgrade** (`apt-get full-upgrade`, keeping existing conffiles). Then build against the new kernel's
   headers. This can run before the reboot, because `KVER`/`KSRC` override the Makefile's `uname -r`
   default:

   ```sh
   K=6.8.12-1029-tegra
   test -d /lib/modules/$K/build || { echo "missing headers for $K"; exit 1; }
   cd ~/rtl8822ce-r39.2.2/source/nvidia-oot/drivers/net/wireless/realtek/rtl8822ce
   make -j4 ARCH=arm64 KVER=$K KSRC=/lib/modules/$K/build \
     srctree.nvidia-oot="$HOME/rtl8822ce-r39.2.2/source/nvidia-oot" \
     USER_EXTRA_CFLAGS='-DCONFIG_REGD_SRC_FROM_OS -DCONFIG_FORCE_SW_CHANNEL_PLAN'
   modinfo -F vermagic ./rtl8822ce.ko   # must name $K, proving the override took effect
   ```

3. Copy the new stock module to `~/rtl8822ce-backup/` before replacing anything. **Reboot** into the new
   kernel; it boots on the stock driver. Rebuild the regulatory shim for it.
4. **Trial** on the running new kernel: `rtl8822ce-trial.sh CANDIDATE STOCK os LOG`, with the stock module
   as the known-good original. Check `iw reg get` and `iw phy` by hand.
5. **Install:** copy the stock module to `rtl8822ce.ko.orig`, install the candidate as `rtl8822ce.ko`
   (0644), then run `depmod -a $K`. Reboot and re-check.

Verification record:
- The build ran on 1021 against the 1029 headers, with the explicit `KVER`/`KSRC` shown above. The
  candidate's vermagic is `6.8.12-1029-tegra SMP preempt mod_unload modversions aarch64`.
  SHA-256 is `401a4a0ee26241a5025c65c5f7e5d737e8519d8d9ea12444c81209e3b6d1626d`. The stock R39.2.2 module
  (`897448534f1e9a40e51c3238e1843cc4c9b2bfd90a6b0c19f19409b52af92b8d`) was copied to
  `~/rtl8822ce-backup/` and then kept as `rtl8822ce.ko.orig` beside the installed one.
- The detached trial ran after rebooting into 1029, in mode `os`, with the stock module as the fallback.
  It passed in 5 s; the script checks the Renzo uplink and an interface-bound ping, and its deadman was cancelled.
  Manual checks afterwards: US/OS regulatory (no self-managed phy), 36–48 without NO_IR, 52+ radar
  detection.
- Installed with `depmod -a 6.8.12-1029-tegra`. After a second reboot, the candidate loaded at boot with
  the same regulatory state and the uplink working.
- Temporary NetworkManager AP on channel 36/5180 MHz: held 5 minutes (65/65 checks), then three
  `aa-system-helper` restarts. The profile was removed and the uplink restored afterwards.
- Live receiver window (A26, production `ap.conf` channel 44, 480 s): projection painted after 13 s and the
  A/V contract passed (p99 +42.0 ms, max +54.7 ms). The receiver-owned AP on channel 36 is still the open item in #73.
- Power mode stayed MAXN_SUPER and `net.core.wmem_max` stayed 8 MiB. The shim/watchdog units
  stayed disabled; the shim was rebuilt for 1029 for rollback only. `wifi-regdom.service` stayed enabled.
