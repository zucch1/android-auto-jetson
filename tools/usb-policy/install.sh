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
#                        system. When non-empty, udevadm is never invoked and
#                        the test hooks below become active.
#   AA_USB_POLICY_USER   Receiver session user for the plugdev membership
#                        check (default: SUDO_USER or the invoking user).
#   AA_USB_POLICY_TEST_HOOKS / AA_USB_POLICY_TEST_HOOK_CMD
#                        Test-only fault injection (fake-root mode ONLY):
#                        when a checkpoint name is listed in _HOOKS, _CMD is
#                        evaluated in this shell at that checkpoint. Used by
#                        tests/usb/test_usb_policy.py to simulate crashes and
#                        TOCTOU swaps at exact points.
#
# Usage: sudo -n bash tools/usb-policy/install.sh
set -euo pipefail

RULES_NAME="70-android-auto-receiver.rules"
STATE_SCHEMA="aa-usb-policy-state/1"
# Must match the port constraint inside ${RULES_NAME}; the trigger is scoped
# to this one port's device tree (device + interfaces + tty ports) only.
QUALIFIED_PORT_SYSNAME="1-2.1"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "${SCRIPT_DIR}/common.sh"
log() { printf '%s\n' "$*"; }
die() { printf 'install.sh: %s\n' "$*" >&2; exit 1; }

ROOT="${AA_USB_POLICY_ROOT:-}"
if [ -n "$ROOT" ]; then
    RESOLVED_ROOT="$(readlink -f -- "$ROOT")" || die "cannot resolve AA_USB_POLICY_ROOT: ${ROOT}"
    [ "$RESOLVED_ROOT" != "/" ] \
        || die "AA_USB_POLICY_ROOT resolves to the live filesystem root; use an empty value for live mode"
fi
SRC="${SCRIPT_DIR}/${RULES_NAME}"
DST="${ROOT}/etc/udev/rules.d/${RULES_NAME}"
STATE_DIR="${ROOT}/var/lib/android-auto-receiver/usb-policy"
MANIFEST="${STATE_DIR}/manifest"
PRIOR="${STATE_DIR}/prior.rules"
USER_NAME="${AA_USB_POLICY_USER:-${SUDO_USER:-$(id -un)}}"

live() { [ -z "$ROOT" ]; }

reload_and_trigger() {
    test_hook before-reload
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
# Capture the source ONCE (finding F3): every later step - policy gate,
# udevadm verify, sha256, install copy - consumes this private snapshot, so a
# source swap between steps can neither install rejected content nor record a
# digest of different bytes than those installed.
trap cleanup_publication EXIT
snapshot_src "$SRC"
python3 "${SCRIPT_DIR}/rules_check.py" verify "$SNAP" || die "rules_check rejected ${RULES_NAME}"
test_hook after-validate
if command -v udevadm >/dev/null 2>&1 && udevadm verify --help >/dev/null 2>&1; then
    timeout 60 udevadm verify --no-style -N never "$SNAP" \
        || die "udevadm verify rejected ${RULES_NAME}"
    log "verify: rules_check + udevadm verify OK"
else
    log "verify: rules_check OK (udevadm verify unavailable on this system)"
fi

SRC_SHA="$(sha_of "$SNAP")"
test_hook after-hash

# Filesystem trust and entry types (finding F1) BEFORE any write: every
# directory from the trust root down must be a real, euid-owned,
# non-group/other-writable directory - so nobody else can swap entries under
# us - and the destination/manifest/backup must be absent or regular
# non-symlink files (a symlinked destination, e.g. a /dev/null udev mask or an
# attacker-planted link, must never receive bytes or a chmod).
trusted_chain_check "$(dirname -- "$DST")"
trusted_chain_check "$STATE_DIR"
check_entry "$DST" "destination"
check_entry "$MANIFEST" "manifest"
check_entry "$PRIOR" "backup"
mkdir -p "$(dirname "$DST")" "$STATE_DIR"
lock_state
test_hook after-check-dst

# Saved-state validation (finding F4, shared with rollback.sh): strict
# manifest parsing (unique keys, exact key set, formats) plus cross-field
# invariants and a state-entry inventory. Inconsistent saved state is refused
# with the destination, state and backup preserved - a flipped prior_existed
# or a surviving backup without its manifest must never delete or re-base the
# original backup.
state_inventory_check
if [ -f "$MANIFEST" ]; then
    state_validate
    case "$PHASE" in
        restoring | restored)
            die "rollback is unfinished (phase=${PHASE}); run tools/usb-policy/rollback.sh to complete it first" ;;
    esac
    DST_SHA="-"
    [ -f "$DST" ] && DST_SHA="$(sha_of "$DST")"
    if [ "$PHASE" = "installed" ] && [ "$DST_SHA" = "$SRC_SHA" ] && [ "$INSTALLED_SHA" = "$SRC_SHA" ]; then
        check_installed_metadata "$DST"
        log "already-installed: ${RULES_NAME} unchanged (sha256 ${SRC_SHA})"
        check_user_access
        reload_and_trigger
        exit 0
    fi
    # Drift, upgrade or completion of an interrupted install: the FIRST
    # install's prior-state record stays authoritative for rollback (finding
    # F5: the phase is recorded durably BEFORE the destructive publish).
    INSTALLED_SHA="$SRC_SHA"
    INSTALLED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    manifest_write installing
    test_hook after-record-installing
    publish_copy "$SNAP" "$DST" 0644 after-stage-dst
    test_hook after-publish-dst
    manifest_write installed
    test_hook after-record-installed
    log "repaired: ${RULES_NAME} updated to sha256 ${SRC_SHA} (prior state unchanged)"
else
    if [ -e "$PRIOR" ] || [ -L "$PRIOR" ]; then
        die "saved backup ${PRIOR} exists without a manifest; refusing to touch state (would re-base the original backup)"
    fi
    # Publish the initial backup and manifest together. Until this rename the
    # destination is unchanged; after it a complete rollback record exists.
    PRIVATE_STATE_TMP="$(mktemp -d -p "$(dirname -- "$STATE_DIR")" .aa-usb-state.XXXXXX)"
    (
        STATE_DIR="$PRIVATE_STATE_TMP"
        MANIFEST="${STATE_DIR}/manifest"
        PRIOR="${STATE_DIR}/prior.rules"
        if [ -f "$DST" ]; then
            publish_copy "$DST" "$PRIOR" preserve
            PRIOR_EXISTED=1
            PRIOR_SHA="$(sha_of "$PRIOR")"
        else
            PRIOR_EXISTED=0
            PRIOR_SHA="-"
        fi
        test_hook after-stage-prior
        INSTALLED_SHA="$SRC_SHA"
        INSTALLED_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        manifest_write installing
        test_hook after-stage-state
    )
    mv -T -- "$PRIVATE_STATE_TMP" "$STATE_DIR"
    PRIVATE_STATE_TMP=""
    state_validate
    test_hook after-record-installing
    publish_copy "$SNAP" "$DST" 0644 after-stage-dst
    test_hook after-publish-dst
    manifest_write installed
    test_hook after-record-installed
    log "installed: ${DST} (sha256 ${SRC_SHA}, prior_existed=${PRIOR_EXISTED})"
fi

check_user_access
reload_and_trigger
log "done: scoped USB device policy installed; rollback with tools/usb-policy/rollback.sh"
