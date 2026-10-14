#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Roll back the scoped Android Auto USB udev policy (tools/usb-policy).
#
# Restores the EXACT prior state recorded by install.sh:
#   - if no rules file existed before install: the installed file is removed,
#   - if a different rules file existed before: its recorded bytes are
#     restored (sha256-verified before and after),
#   - udev is reloaded and the qualified port's device tree is re-triggered so
#     rule-derived udev properties (e.g. ID_MM_DEVICE_IGNORE) are re-evaluated
#     and cleared with the removed rules; named-user ACLs are NOT claimed to
#     be revoked by reload+trigger (see README "Rollback" for the exact
#     mechanism and the limitation),
#   - the install manifest and backup are removed: the system is left exactly
#     as before the install, with no trace of this tooling.
#
# Idempotent: a second run with no recorded state is a clean no-op (exit 0).
# Fail closed: an unknown/corrupt manifest, a backup that does not match its
# recorded hash, a symlinked destination, or an installed file that drifted
# from the manifest's installed_sha256 aborts WITHOUT deleting anything.
#
# Environment:
#   AA_USB_POLICY_ROOT   Alternate filesystem root (tests). Empty = live
#                        system. When non-empty, udevadm is never invoked.
#
# Usage: sudo -n bash tools/usb-policy/rollback.sh
set -euo pipefail

RULES_NAME="70-android-auto-receiver.rules"
STATE_SCHEMA="aa-usb-policy-state/1"
QUALIFIED_PORT_SYSNAME="1-2.1"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/common.sh"
ROOT="${AA_USB_POLICY_ROOT:-}"
DST="${ROOT}/etc/udev/rules.d/${RULES_NAME}"
STATE_DIR="${ROOT}/var/lib/android-auto-receiver/usb-policy"
MANIFEST="${STATE_DIR}/manifest"
PRIOR="${STATE_DIR}/prior.rules"

log() { printf '%s\n' "$*"; }
die() { printf 'rollback.sh: %s\n' "$*" >&2; exit 1; }

live() { [ -z "$ROOT" ]; }

reload_and_trigger() {
    test_hook before-reload
    if ! live; then
        log "udev-skip: non-live root '${ROOT}' (no udevadm calls;" \
            "named-user ACLs are NOT revoked by this tooling - see README 'Rollback')"
        return 0
    fi
    if ! command -v udevadm >/dev/null 2>&1; then
        die "udevadm not available on a live root; refusing to roll back without clearing device state"
    fi
    timeout 60 udevadm control --reload || die "udevadm control --reload failed"
    local syspath
    if syspath="$(readlink -f "/sys/bus/usb/devices/${QUALIFIED_PORT_SYSNAME}" 2>/dev/null)" \
        && [ -n "$syspath" ] && [ -d "$syspath" ]; then
        timeout 60 udevadm trigger --action=change --parent-match="$syspath" \
            || die "udevadm trigger failed"
        timeout 30 udevadm settle --timeout=10 || die "udevadm settle failed"
        log "udev-reloaded: re-triggered device tree of ${QUALIFIED_PORT_SYSNAME}" \
            "(rule-derived properties like ID_MM_DEVICE_IGNORE are re-evaluated;" \
            "named-user ACLs are NOT revoked by this tooling - see README 'Rollback')"
    else
        log "udev-reloaded: qualified port ${QUALIFIED_PORT_SYSNAME} absent; no trigger needed"
    fi
}

trap cleanup_publication EXIT

# Filesystem trust and entry types (finding F1) BEFORE any mutation: the
# destination/manifest/backup must be absent or regular non-symlink files and
# every directory from the trust root down must be a real, euid-owned,
# non-group/other-writable directory. A dangling manifest symlink must never
# be mistaken for "no state" and must never be written through.
trusted_chain_check "$(dirname -- "$DST")"
trusted_chain_check "$STATE_DIR"
check_entry "$MANIFEST" "manifest"
check_entry "$PRIOR" "backup"
check_entry "$DST" "destination"
mkdir -p "$STATE_DIR"
lock_state
state_inventory_check

if [ ! -f "$MANIFEST" ]; then
    if [ -e "$PRIOR" ] || [ -L "$PRIOR" ]; then
        die "saved backup ${PRIOR} exists without a manifest; refusing to touch state"
    fi
    rmdir "$STATE_DIR" 2>/dev/null || true
    rmdir "${ROOT}/var/lib/android-auto-receiver" 2>/dev/null || true
    log "nothing-to-rollback: no install state at ${STATE_DIR}"
    exit 0
fi

# Saved-state validation (finding F4, shared with install.sh): strict
# manifest parsing and cross-field invariants; inconsistent state is refused
# with the destination, state and backup preserved.
state_validate

DST_SHA="-"
[ -f "$DST" ] && DST_SHA="$(sha_of "$DST")"
if [ "$PRIOR_EXISTED" = "1" ]; then
    BASELINE_SHA="$PRIOR_SHA"
else
    BASELINE_SHA="-"
fi

# Transaction recovery (finding F5): the phase durably recorded before each
# destructive change tells us what a previous run completed. phase=installed
# still requires the exact installed artifact (third-party drift is refused
# exactly as before); restoring/installing accept only the installed artifact
# or the recorded baseline; restored means the file is back and only udev
# reconciliation may still be pending.
need_restore=0
case "$PHASE" in
    installed)
        [ -f "$DST" ] \
            || die "installed rules file ${DST} is missing (expected sha256 ${INSTALLED_SHA}); refusing to touch state"
        [ "$DST_SHA" = "$INSTALLED_SHA" ] \
            || die "installed rules file drifted (manifest ${INSTALLED_SHA}, got ${DST_SHA}); refusing to overwrite/delete someone else's edits"
        need_restore=1
        ;;
    installing | restoring)
        if [ "$DST_SHA" = "$INSTALLED_SHA" ]; then
            need_restore=1
        elif [ "$DST_SHA" = "$BASELINE_SHA" ]; then
            log "recovered: destination already matches the recorded prior state (phase=${PHASE})"
        else
            die "destination matches neither the installed policy nor the recorded prior state (got ${DST_SHA}, phase=${PHASE}); refusing to touch state"
        fi
        ;;
    restored)
        [ "$DST_SHA" = "$BASELINE_SHA" ] \
            || die "destination changed after the recorded restore (got ${DST_SHA}, want ${BASELINE_SHA}); refusing to touch state"
        log "recovered: restore already recorded (phase=restored); completing udev reconciliation"
        ;;
esac

test_hook after-check-dst
if [ "$need_restore" = "1" ]; then
    # Durably record the restore BEFORE the destructive change (finding F5):
    # an interrupted or reload-failed rollback is then distinguishable from a
    # completed one and a rerun completes it instead of refusing on drift.
    manifest_write restoring
    test_hook after-record-restoring
    if [ "$PRIOR_EXISTED" = "1" ]; then
        [ -f "$PRIOR" ] || die "manifest records a prior rules file but ${PRIOR} is missing"
        ACTUAL_PRIOR_SHA="$(sha_of "$PRIOR")"
        [ "$ACTUAL_PRIOR_SHA" = "$PRIOR_SHA" ] \
            || die "prior backup hash mismatch (recorded ${PRIOR_SHA}, got ${ACTUAL_PRIOR_SHA}); refusing to restore"
        publish_copy "$PRIOR" "$DST" preserve after-stage-dst
        [ "$(sha_of "$DST")" = "$PRIOR_SHA" ] || die "restored file hash mismatch after copy"
        log "restored: prior ${RULES_NAME} (sha256 ${PRIOR_SHA})"
    else
        if [ -L "$DST" ] || { [ -e "$DST" ] && [ ! -f "$DST" ]; }; then
            die "destination ${DST} changed during rollback; refusing to remove"
        fi
        rm -f "$DST"
        log "removed: installed ${RULES_NAME} (no prior file existed)"
    fi
    test_hook after-restore-dst
    manifest_write restored
    test_hook after-record-restored
fi

reload_and_trigger
test_hook after-reload

# Retire the entire paired record atomically only after restore and reload.
# An interruption during disposal cannot strand an active half-manifest.
PRIVATE_STATE_TMP="$(mktemp -d -p "$(dirname -- "$STATE_DIR")" .aa-usb-retired.XXXXXX)"
mv -T -- "$STATE_DIR" "$PRIVATE_STATE_TMP"
test_hook after-retire-state
rm -rf -- "${PRIVATE_STATE_TMP:?}"
PRIVATE_STATE_TMP=""
rmdir "${ROOT}/var/lib/android-auto-receiver" 2>/dev/null || true
log "done: prior state restored; install state removed"
