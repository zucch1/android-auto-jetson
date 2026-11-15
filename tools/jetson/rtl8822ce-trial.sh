#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run as root via systemd-run over direct Ethernet. Does not install modules.
set -u
if [[ $EUID != 0 || $# != 4 ]]; then
    echo "usage (root): $0 CANDIDATE.ko ORIGINAL.ko os|vendor LOG" >&2
    exit 2
fi
candidate=$(realpath -e "$1") || exit 2
original=$(realpath -e "$2") || exit 2
mode=$3
[[ $mode == os || $mode == vendor ]] || exit 2
exec >>"$4" 2>&1
log() { printf '%s %s\n' "$(date --iso-8601=seconds)" "$*"; }
# Bound recovery too: a wedged tool cannot consume the management window.
run() { timeout -k 2 10 "$@"; }
health() {
    local until=$((SECONDS + 60))
    while (( SECONDS < until )); do
        if timeout 3 nmcli -t -f NAME,DEVICE connection show --active | grep -qx 'Renzo:wlP1p1s0' &&
           timeout 3 ping -I wlP1p1s0 -c 1 -W 1 1.1.1.1; then return 0; fi
        sleep 1
    done
    return 1
}
load() {
    local module=$1 regulatory=$2
    run systemctl stop rtl8822ce-regd-watchdog.service rtl8822ce-regd-shim.service || return 1
    if [[ -d /sys/module/rtl8822ce_regd_shim ]]; then run rmmod rtl8822ce_regd_shim || return 1; fi
    if [[ -d /sys/module/rtl8822ce ]]; then run rmmod rtl8822ce || return 1; fi
    local params=(rtw_country_code=US rtw_power_mgnt=0 rtw_ips_mode=0 rtw_smart_ps=0
        rtw_pci_aspm_enable=0 rtw_pci_dynamic_aspm_linkctrl=0 rtw_lps_level=0)
    if [[ $regulatory == os ]]; then
        params+=("rtw_excl_chs=52,56,60,64,100,104,108,112,116,120,124,128,132,136,140,144")
    fi
    run insmod "$module" "${params[@]}" || return 1
    run iw reg set US || return 1
    if [[ $regulatory == vendor ]]; then
        run systemctl restart rtl8822ce-regd-shim.service rtl8822ce-regd-watchdog.service || return 1
    fi
    run systemctl restart wpa_supplicant.service NetworkManager.service || return 1
    run nmcli --wait 0 connection up Renzo || true
}
# Last-resort reboot uses the already installed module, never the candidate path.
# Only use this before candidate installation, or with a verified installed module.
deadman=aa-wifi-deadman-$$
run systemd-run --unit="$deadman" --on-active=170 /usr/bin/systemctl reboot || exit 2
recovered() { run systemctl stop "$deadman.timer"; }
log "candidate=$candidate mode=$mode original=$original"
sha256sum "$candidate" "$original"
if load "$candidate" "$mode" && health; then
    recovered || { log DEADMAN_CANCEL_FAILED; exit 1; }
    log TRIAL_OK
    cat /proc/net/rtl88x2ce/wlP1p1s0/chan_plan
    exit 0
fi
log 'TRIAL_FAILED: restoring original'
if load "$original" vendor && health; then recovered || log DEADMAN_CANCEL_FAILED; log ORIGINAL_RECOVERED; exit 1; fi
log 'RECOVERY_FAILED: rebooting with installed module'
run systemctl reboot
exit 1
