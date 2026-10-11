#!/bin/bash
# rtl8822ce-regd-watchdog — re-assert the US regd whenever the vendor driver's mode
# transitions clobber the shim's one-shot registration (drift observed after AP/STA
# qual cycles: phy#0 reverts to `country 00` while the shim module stays loaded).
# Mechanism: rmmod + modprobe rtl8822ce_regd_shim (its init re-registers the US domain).
# Convergence: checked every 5 s. Logs to syslog (tag: regd-watchdog) and journal.
while true; do
  if ! iw reg get 2>/dev/null | sed -n '/self-managed/,$p' | head -3 | grep -q "country US"; then
    logger -t regd-watchdog "drift detected (phy#0 != country US) — re-asserting"
    if rmmod rtl8822ce_regd_shim 2>/dev/null; then
      if modprobe rtl8822ce_regd_shim 2>/dev/null; then
        STATE=$(iw reg get 2>/dev/null | sed -n '/self-managed/,$p' | head -2 | tr '\n' ' ')
        logger -t regd-watchdog "re-assert ok: ${STATE}"
      else
        logger -t regd-watchdog "ERROR: modprobe rtl8822ce_regd_shim failed"
      fi
    else
      logger -t regd-watchdog "WARN: rmmod failed (module busy) — will retry next cycle"
    fi
  fi
  sleep 5
done
