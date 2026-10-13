#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Install the scoped Android Auto USB udev policy (tools/usb-policy).
#
# Bounded and idempotent:
#   - validates the rules file first (rules_check.py + `udevadm verify`) and
#     FAILS CLOSED on any malformed input before touching system state,
#   - records the exact prior state in a manifest so rollback.sh can restore
#     it byte-exactly,
#   - re-running on an unchanged install is a no-op besides a udev reload,
#   - re-running after drift repairs the installed file while keeping the
#     FIRST install's prior-state record authoritative for rollback.
#
# Environment:
#   AA_USB_POLICY_ROOT   Alternate filesystem root (tests). Empty = live
#                        system. When non-empty, udevadm is never invoked.
#   AA_USB_POLICY_USER   Receiver session user for the plugdev membership
#                        check (default: SUDO_USER or the invoking user).
#
# Usage: sudo -n bash tools/usb-policy/install.sh
set -euo pipefail

RULES_NAME="70-android-auto-receiver.rules"
STATE_SCHEMA="aa-usb-policy-state/1"
# Must match the port constraint inside ${RULES_NAME}; the trigger is scoped
# to this one port's device tree (device + interfaces + tty ports) only.
QUALIFIED_PORT_SYSNAME="1-2.1"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${AA_USB_POLICY_ROOT:-}"
SRC="${SCRIPT_DIR}/${RULES_NAME}"
DST="${ROOT}/etc/udev/rules.d/${RULES_NAME}"
STATE_DIR="${ROOT}/var/lib/android-auto-receiver/usb-policy"
MANIFEST="${STATE_DIR}/manifest"
PRIOR="${STATE_DIR}/prior.rules"
USER_NAME="${AA_USB_POLICY_USER:-${SUDO_USER:-$(id -un)}}"

log() { printf '%s\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

sha_of() { sha256sum "$1" | awk '{print $1}'; }

manifest_get() {
    # Print manifest value for key $1; empty if absent.
    sed -n "s/^$1=//p" "$MANIFEST" 2>/dev/null | head -n 1
}

live() { [ -z "$ROOT" ]; }

reload_and_trigger() {
    if ! live; then
        log "udev-skip: non-live root '${ROOT}' (no udevadm calls)"
        return 0
    fi
    if ! command -v udevadm >/dev/null 2>&1; then
        die "udevadm not available on a live root; refusing to install without a reload path"
    fi
    timeout 60 udevadm control --reload || die "udevadm control --reload failed"
    local syspath
    if syspath="$(readlink -f "/sys/bus/usb/devices/${QUALIFIED_PORT_SYSNAME}" 2>/dev/null)" \
        && [ -n "$syspath" ] && [ -d "$syspath" ]; then
        # --parent-match covers the port device and every descendant
        # (interfaces, tty ports); nothing outside the qualified tree is
        # reprocessed.
        timeout 60 udevadm trigger --action=change --parent-match="$syspath" \
            || die "udevadm trigger failed"
        timeout 30 udevadm settle --timeout=10 || die "udevadm settle failed"
        log "udev-reloaded: triggered device tree of ${QUALIFIED_PORT_SYSNAME}"
    else
        log "udev-reloaded: qualified port ${QUALIFIED_PORT_SYSNAME} absent; no trigger needed"
    fi
}

check_user_access() {
    live || return 0
    if id -nG "${USER_NAME}" 2>/dev/null | tr ' ' '\n' | grep -qx plugdev; then
        log "user-check: '${USER_NAME}' is a plugdev member (bulk-pipe group access OK)"
    else
        log "user-check: WARNING '${USER_NAME}' is not a plugdev member;" \
            "device-node GROUP=plugdev will not grant access." \
            "One-time prerequisite: sudo usermod -aG plugdev '${USER_NAME}'" \
            "(TAG+=uaccess still covers the active seat session)."
    fi
}

# --- validate first: no system state is touched on failure -----------------
[ -f "$SRC" ] || die "missing rules file: $SRC"
if ! command -v python3 >/dev/null 2>&1; then
    die "python3 is required for the fail-closed rules syntax gate"
fi
python3 "${SCRIPT_DIR}/rules_check.py" verify "$SRC" || die "rules_check rejected ${RULES_NAME}"
if command -v udevadm >/dev/null 2>&1 && udevadm verify --help >/dev/null 2>&1; then
    timeout 60 udevadm verify --no-style -N never "$SRC" \
        || die "udevadm verify rejected ${RULES_NAME}"
    log "verify: rules_check + udevadm verify OK"
else
    log "verify: rules_check OK (udevadm verify unavailable on this system)"
fi

SRC_SHA="$(sha_of "$SRC")"

# Refuse a symlinked destination BEFORE any write or chmod: `cp -f` and
# `chmod` follow a pre-existing symlink (dangling or valid) and would
# overwrite or re-mode whatever it points at - e.g. a symlink to /dev/null
# (a legitimate udev mask) or to an ordinary file.
if [ -L "$DST" ]; then
    die "destination ${DST} is a symlink (target: $(readlink "$DST")); refusing to install through a link"
fi

mkdir -p "$(dirname "$DST")"

if [ -f "$MANIFEST" ]; then
    schema="$(manifest_get schema)"
    [ "$schema" = "$STATE_SCHEMA" ] \
        || die "unrecognized manifest schema '${schema}' in ${MANIFEST}; refusing to touch state"
    [ "$(manifest_get rules_name)" = "$RULES_NAME" ] \
        || die "manifest is for a different rules file; refusing to touch state"
    PRIOR_EXISTED="$(manifest_get prior_existed)"
    PRIOR_SHA="$(manifest_get prior_sha256)"
    INSTALLED_SHA="$(manifest_get installed_sha256)"
    [ -n "$INSTALLED_SHA" ] || die "manifest lacks installed_sha256"
    # Validate the COMPLETE saved rollback state before claiming success or
    # repairing: corrupt/missing prior state must never be carried forward.
    case "$PRIOR_EXISTED" in
        0 | 1) ;;
        *) die "manifest prior_existed must be 0 or 1 (got '${PRIOR_EXISTED}'); refusing to touch state" ;;
    esac
    if [ -z "$PRIOR_SHA" ] || ! printf '%s' "$PRIOR_SHA" | grep -Eq '^(-|[0-9a-f]{64})$'; then
        die "manifest prior_sha256 missing or not a sha256/sentinel (got '${PRIOR_SHA}'); refusing to touch state"
    fi
    if [ "$PRIOR_EXISTED" = "1" ]; then
        [ ! -L "$PRIOR" ] || die "backup ${PRIOR} is a symlink; refusing to trust it as the recorded prior state"
        [ -f "$PRIOR" ] || die "manifest records a prior rules file but ${PRIOR} is missing; refusing to touch state"
        ACTUAL_PRIOR_SHA="$(sha_of "$PRIOR")"
        [ "$ACTUAL_PRIOR_SHA" = "$PRIOR_SHA" ] \
            || die "prior backup hash mismatch (recorded ${PRIOR_SHA}, got ${ACTUAL_PRIOR_SHA}); refusing to touch state"
    fi
    DST_SHA="-"
    [ -f "$DST" ] && DST_SHA="$(sha_of "$DST")"
    if [ "$DST_SHA" = "$SRC_SHA" ] && [ "$INSTALLED_SHA" = "$SRC_SHA" ]; then
        log "already-installed: ${RULES_NAME} unchanged (sha256 ${SRC_SHA})"
        check_user_access
        reload_and_trigger
        exit 0
    fi
    # Drift or upgrade: repair the installed file; the FIRST install's
    # prior-state record stays authoritative for rollback.
    cp -f "$SRC" "$DST"
    chmod 0644 "$DST"
    {
        printf 'schema=%s\n' "$STATE_SCHEMA"
        printf 'rules_name=%s\n' "$RULES_NAME"
        printf 'prior_existed=%s\n' "$PRIOR_EXISTED"
        printf 'prior_sha256=%s\n' "$PRIOR_SHA"
        printf 'installed_sha256=%s\n' "$SRC_SHA"
        printf 'installed_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$MANIFEST"
    log "repaired: ${RULES_NAME} updated to sha256 ${SRC_SHA} (prior state unchanged)"
else
    mkdir -p "$STATE_DIR"
    if [ -f "$DST" ]; then
        [ ! -L "$PRIOR" ] || die "backup ${PRIOR} is a symlink; refusing to write the prior backup through a link"
        cp -p "$DST" "$PRIOR"
        PRIOR_EXISTED=1
        PRIOR_SHA="$(sha_of "$PRIOR")"
    else
        PRIOR_EXISTED=0
        PRIOR_SHA="-"
    fi
    cp -f "$SRC" "$DST"
    chmod 0644 "$DST"
    {
        printf 'schema=%s\n' "$STATE_SCHEMA"
        printf 'rules_name=%s\n' "$RULES_NAME"
        printf 'prior_existed=%s\n' "$PRIOR_EXISTED"
        printf 'prior_sha256=%s\n' "$PRIOR_SHA"
        printf 'installed_sha256=%s\n' "$SRC_SHA"
        printf 'installed_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    } >"$MANIFEST"
    log "installed: ${DST} (sha256 ${SRC_SHA}, prior_existed=${PRIOR_EXISTED})"
fi

check_user_access
reload_and_trigger
log "done: scoped USB device policy installed; rollback with tools/usb-policy/rollback.sh"
