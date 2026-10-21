#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
# rtl8822ce-regd-watchdog — re-assert the US regd whenever the vendor driver's mode
# transitions clobber the shim's one-shot registration (drift observed after AP/STA
# qual cycles: phy#0 reverts to `country 00` while the shim module stays loaded).
#
# Mechanism: reload rtl8822ce_regd_shim (its init re-registers the US domain).
# Self-healing across loader failures: a PRESENT module is unloaded then reloaded;
# an ABSENT module is loaded directly; a failed load or a busy unload is retried
# on later cycles (the loop never gives up). Poll interval: 5 s + command time.
#
# Scope note: the drift check resolves the wiphy from $IFACE (arg 1, default
# wlP1p1s0) and inspects that phy's section — it does not assume the first
# self-managed section, so a second self-managed adapter cannot mask drift.
IFACE="${1:-wlP1p1s0}"

resolve_wiphy() {
  iw dev "$IFACE" info 2>/dev/null | sed -n 's/.*wiphy \([0-9][0-9]*\).*/\1/p'
}

target_country() {
  iw reg get 2>/dev/null | awk -v p="phy#${WIPHY_IDX} (self-managed)" '
    $0 == p { inphy = 1; next }
    /^phy#/ { inphy = 0 }
    inphy && $1 == "country" && !found { print $2; found = 1 }
  '
}

verify_recovery() {
  WIPHY_IDX=$(resolve_wiphy) && [ -n "$WIPHY_IDX" ] &&
    COUNTRY=$(target_country) && [ "$COUNTRY" = "US:" ]
}

while true; do
  if ! WIPHY_IDX=$(resolve_wiphy) || [ -z "$WIPHY_IDX" ]; then
    logger -t regd-watchdog "WARN: cannot resolve wiphy for $IFACE — retry next cycle"
    sleep 5
    continue
  fi
  COUNTRY=$(target_country) || COUNTRY=""
  if [ "$COUNTRY" != "US:" ]; then
    logger -t regd-watchdog "drift detected (phy#${WIPHY_IDX} != country US) — re-asserting"
    if lsmod 2>/dev/null | grep '^rtl8822ce_regd_shim' >/dev/null; then
      if rmmod rtl8822ce_regd_shim 2>/dev/null; then
        if modprobe rtl8822ce_regd_shim 2>/dev/null; then
          if verify_recovery; then
            logger -t regd-watchdog "re-assert ok: country ${COUNTRY}"
          else
            logger -t regd-watchdog "WARN: recovery verification failed for $IFACE — retry next cycle"
          fi
        else
          logger -t regd-watchdog "ERROR: modprobe failed after unload — retry next cycle"
        fi
      else
        logger -t regd-watchdog "WARN: rmmod failed (module busy) — retry next cycle"
      fi
    else
      if modprobe rtl8822ce_regd_shim 2>/dev/null; then
        if verify_recovery; then
          logger -t regd-watchdog "load ok (module was absent): country ${COUNTRY}"
        else
          logger -t regd-watchdog "WARN: recovery verification failed for $IFACE — retry next cycle"
        fi
      else
        logger -t regd-watchdog "ERROR: modprobe failed (module absent) — retry next cycle"
      fi
    fi
  fi
  sleep 5
done
