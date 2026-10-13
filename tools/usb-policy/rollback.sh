#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Roll back the scoped Android Auto USB udev policy (tools/usb-policy).
#
# Restores the EXACT prior state recorded by install.sh:
#   - if no rules file existed before install: the installed file is removed,
#   - if a different rules file existed before: its recorded bytes are
#     restored (sha256-verified before and after),
#   - udev is reloaded and the qualified port's device tree is re-triggered so
#     no ACL or ID_MM_DEVICE_IGNORE state survives the removed rules,
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
ROOT="${AA_USB_POLICY_ROOT:-}"
DST="${ROOT}/etc/udev/rules.d/${RULES_NAME}"
STATE_DIR="${ROOT}/var/lib/android-auto-receiver/usb-policy"
MANIFEST="${STATE_DIR}/manifest"
PRIOR="${STATE_DIR}/prior.rules"

log() { printf '%s\n' "$*"; }
die() { printf 'rollback.sh: %s\n' "$*" >&2; exit 1; }

sha_of() { sha256sum "$1" | awk '{print $1}'; }

manifest_get() {
    sed -n "s/^$1=//p" "$MANIFEST" 2>/dev/null | head -n 1
}

live() { [ -z "$ROOT" ]; }

reload_and_trigger() {
    if ! live; then
        log "udev-skip: non-live root '${ROOT}' (no udevadm calls)"
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
            "(stale ACLs/ID_MM_DEVICE_IGNORE cannot survive reload+trigger)"
    else
        log "udev-reloaded: qualified port ${QUALIFIED_PORT_SYSNAME} absent; no trigger needed"
    fi
}

if [ ! -f "$MANIFEST" ]; then
    log "nothing-to-rollback: no install state at ${STATE_DIR}"
    exit 0
fi

schema="$(manifest_get schema)"
[ "$schema" = "$STATE_SCHEMA" ] \
    || die "unrecognized manifest schema '${schema}'; refusing to touch state"
[ "$(manifest_get rules_name)" = "$RULES_NAME" ] \
    || die "manifest is for a different rules file; refusing to touch state"
PRIOR_EXISTED="$(manifest_get prior_existed)"
PRIOR_SHA="$(manifest_get prior_sha256)"
case "$PRIOR_EXISTED" in
    0 | 1) ;;
    *) die "manifest prior_existed must be 0 or 1 (got '${PRIOR_EXISTED}')" ;;
esac

INSTALLED_SHA="$(manifest_get installed_sha256)"
[ -n "$INSTALLED_SHA" ] || die "manifest lacks installed_sha256; refusing to touch state"
if [ -L "$DST" ]; then
    die "destination ${DST} is a symlink (target: $(readlink "$DST")); refusing to roll back through a link"
fi
# The present destination must still be the exact artifact install.sh
# installed: anyone else's edits must never be silently overwritten or
# deleted by a rollback.
if [ ! -f "$DST" ]; then
    die "installed rules file ${DST} is missing (expected sha256 ${INSTALLED_SHA}); refusing to touch state"
fi
ACTUAL_DST_SHA="$(sha_of "$DST")"
[ "$ACTUAL_DST_SHA" = "$INSTALLED_SHA" ] \
    || die "installed rules file drifted (manifest ${INSTALLED_SHA}, got ${ACTUAL_DST_SHA}); refusing to overwrite/delete someone else's edits"

if [ "$PRIOR_EXISTED" = "1" ]; then
    [ ! -L "$PRIOR" ] || die "backup ${PRIOR} is a symlink; refusing to restore through a link"
    [ -f "$PRIOR" ] || die "manifest records a prior rules file but ${PRIOR} is missing"
    ACTUAL_PRIOR_SHA="$(sha_of "$PRIOR")"
    [ "$ACTUAL_PRIOR_SHA" = "$PRIOR_SHA" ] \
        || die "prior backup hash mismatch (recorded ${PRIOR_SHA}, got ${ACTUAL_PRIOR_SHA}); refusing to restore"
    cp -p "$PRIOR" "$DST"
    [ "$(sha_of "$DST")" = "$PRIOR_SHA" ] || die "restored file hash mismatch after copy"
    log "restored: prior ${RULES_NAME} (sha256 ${PRIOR_SHA})"
else
    rm -f "$DST"
    log "removed: installed ${RULES_NAME} (no prior file existed)"
fi

reload_and_trigger

rm -f "$MANIFEST" "$PRIOR"
rmdir "$STATE_DIR" 2>/dev/null || true
rmdir "${ROOT}/var/lib/android-auto-receiver" 2>/dev/null || true
log "done: prior state restored; install state removed"
