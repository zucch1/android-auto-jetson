#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Shared helpers for tools/usb-policy/{install.sh,rollback.sh}. Sourced only.
#
# Every helper fails closed through the sourcing script's die(). The helpers
# exist so both scripts enforce the SAME filesystem-trust, publication and
# saved-state rules instead of drifting apart.
#
# Threat model: the scripts run as root against a destination and state
# directory that may be attacker-influenced wherever writable, with TOCTOU
# races between any check and its use. Defenses here:
#   1. ancestry trust: every directory from the trust root down to the target
#      must be a real directory owned by the effective uid and not
#      group/other-writable - so nobody else can swap entries at all;
#   2. entry type checks: manifest/backup/destination must be absent or
#      regular files - never symlinks (dangling or valid), FIFOs or dirs;
#   3. staged publication: content is written and chmod'ed into a private
#      temp file first, then renamed into place. The destination is NEVER
#      reopened for writing or chmod - a path swapped under us cannot receive
#      bytes or modes, and rename(2) replaces a swapped link instead of
#      following it.

# Directories these scripts create must come out 0755 whatever the caller's
# umask is: the trust checks above reject group/other-writable directories.
umask 022

# test_hook NAME - test-only fault injection. Evaluated ONLY in fake-root mode
# (AA_USB_POLICY_ROOT non-empty) when NAME is listed in
# AA_USB_POLICY_TEST_HOOKS; the code in AA_USB_POLICY_TEST_HOOK_CMD runs in
# the current shell so it can exit mid-sequence (simulated crash) or mutate
# paths (simulated TOCTOU swap). Never active on a live root.
test_hook() {
    [ -n "${ROOT:-}" ] || return 0
    case " ${AA_USB_POLICY_TEST_HOOKS:-} " in
        *" $1 "*)
            eval "${AA_USB_POLICY_TEST_HOOK_CMD:?AA_USB_POLICY_TEST_HOOK_CMD must be set when AA_USB_POLICY_TEST_HOOKS is used}"
            ;;
    esac
}

# snapshot_src SRC - capture SRC ONCE into private staging (0700, euid-owned)
# and never read the original path again. Validation, hashing and installation
# must all consume the same captured bytes: a source swap between any two of
# those steps can otherwise install content that was never validated or record
# a digest that does not match the installed bytes. Sets SNAP_DIR and SNAP;
# the caller must clean SNAP_DIR up on exit.
snapshot_src() {
    local src="$1"
    SNAP_DIR="$(mktemp -d)" || die "cannot create private staging"
    SNAP="${SNAP_DIR}/$(basename -- "$src")"
    cp -- "$src" "$SNAP" || die "cannot capture source ${src}"
}

sha_of() {
    if [ -L "$1" ]; then
        die "refusing to hash a symlink: $1"
    fi
    sha256sum "$1" | awk '{print $1}'
}

# check_entry FILE LABEL - FILE must be absent or a regular non-symlink.
check_entry() {
    local f="$1" label="$2"
    if [ -L "$f" ]; then
        die "${label} ${f} is a symlink (target: $(readlink -- "$f")); refusing to touch state"
    fi
    if [ -e "$f" ] && [ ! -f "$f" ]; then
        die "${label} ${f} is not a regular file; refusing to touch state"
    fi
}

check_installed_metadata() {
    local f="$1" owner perms expected_owner=0
    # Fake roots model root ownership with the test runner's effective uid.
    [ -z "$ROOT" ] || expected_owner="$(id -u)"
    check_entry "$f" "installed rules file"
    owner="$(stat -c %u -- "$f")"
    perms="$(stat -c %a -- "$f")"
    [ "$owner" = "$expected_owner" ] && [ "$perms" = "644" ] \
        || die "installed rules file ${f} has unsafe metadata (uid ${owner}, mode ${perms}; expected uid ${expected_owner}, mode 644); refusing"
}

check_dir_trusted() {
    local d="$1" label="$2" owner perms
    if [ -L "$d" ]; then
        die "${label} directory ${d} is a symlink; refusing"
    fi
    if [ ! -d "$d" ]; then
        die "${label} directory ${d} is not a directory; refusing"
    fi
    owner="$(stat -c %u -- "$d")"
    [ "$owner" = "$(id -u)" ] \
        || die "${label} directory ${d} is owned by uid ${owner}, not $(id -u); refusing"
    perms="$(stat -c %a -- "$d")"
    if [ $(( 8#$perms & 8#22 )) -ne 0 ]; then
        die "${label} directory ${d} is group/other-writable (mode ${perms}); refusing"
    fi
}

# trusted_chain_check DIR - every existing component from the trust root down
# to DIR must be a trusted directory (see check_dir_trusted). Missing tail
# components are created later by the caller; the deepest existing ancestor is
# then trusted and nobody else can create entries beneath it. The trust root is
# the fake root in tests, the live filesystem root otherwise.
trusted_chain_check() {
    local target="$1" root rel cur comp
    root="${ROOT:-/}"
    root="${root%/}"
    [ -n "$root" ] || root="/"
    if [ "$root" = "/" ]; then
        rel="${target#/}"
    else
        case "$target" in
            "$root") rel="" ;;
            "$root"/*) rel="${target#"$root"/}" ;;
            *) die "internal: ${target} is outside the trust root ${root}" ;;
        esac
    fi
    check_dir_trusted "$root" "trust root"
    cur="$root"
    local IFS='/'
    for comp in $rel; do
        if [ "$cur" = "/" ]; then
            cur="/${comp}"
        else
            cur="${cur}/${comp}"
        fi
        if [ ! -e "$cur" ] && [ ! -L "$cur" ]; then
            break
        fi
        check_dir_trusted "$cur" "ancestor"
    done
}

# publish_copy SRC DST [MODE|preserve] [HOOK] - copy SRC to DST atomically:
# stage next to DST, chmod the staged file (never DST), re-check DST, then
# rename(2) via `mv -fT`. The staged file is removed on any failure.
publish_copy() {
    local src="$1" dst="$2" mode="${3:-0644}" hook="${4:-}"
    local dir base staged
    if [ -L "$src" ]; then
        die "refusing to copy from a symlinked source: ${src}"
    fi
    dir="$(dirname -- "$dst")"
    base="$(basename -- "$dst")"
    staged="$(mktemp -p "$dir" ".aa-usb-policy.XXXXXX")" || die "cannot stage ${dst}"
    PUBLISH_TMP="$staged"
    if [ "$mode" = "preserve" ]; then
        cp -p -- "$src" "$staged" || die "cannot stage ${src}"
    else
        cp -- "$src" "$staged" || die "cannot stage ${src}"
        test_hook after-copy-stage
        chmod "$mode" "$staged" || die "cannot chmod staged ${base}"
    fi
    if [ -n "$hook" ]; then
        test_hook "$hook"
    fi
    check_entry "$dst" "destination"
    mv -fT -- "$staged" "$dst" || die "cannot publish ${dst}"
    PUBLISH_TMP=""
}

# publish_stdin DST [HOOK] - atomically publish stdin content to DST (0644).
publish_stdin() {
    local dst="$1" hook="${2:-}"
    local dir base staged
    dir="$(dirname -- "$dst")"
    base="$(basename -- "$dst")"
    staged="$(mktemp -p "$dir" ".aa-usb-policy.XXXXXX")" || die "cannot stage ${dst}"
    PUBLISH_TMP="$staged"
    cat >"$staged" || die "cannot stage ${base}"
    chmod 0644 "$staged" || die "cannot chmod staged ${base}"
    if [ -n "$hook" ]; then
        test_hook "$hook"
    fi
    check_entry "$dst" "destination"
    mv -fT -- "$staged" "$dst" || die "cannot publish ${dst}"
    PUBLISH_TMP=""
}

# cleanup_publication - remove private staging leftovers (EXIT trap helper).
cleanup_publication() {
    if [ -n "${PUBLISH_TMP:-}" ]; then
        rm -f -- "${PUBLISH_TMP:?}"
    fi
    if [ -n "${SNAP_DIR:-}" ]; then
        rm -rf -- "${SNAP_DIR:?}"
    fi
    if [ -n "${PRIVATE_STATE_TMP:-}" ]; then
        rm -rf -- "${PRIVATE_STATE_TMP:?}"
    fi
}

# state_inventory_check - the state dir may hold ONLY manifest, prior.rules
# and our own staging leftovers; every entry must be a regular non-symlink
# file. Anything else is inconsistent saved state (findings F1/F4) and is
# refused with everything preserved.
state_inventory_check() {
    local entry base
    [ -d "$STATE_DIR" ] || return 0
    for entry in "$STATE_DIR"/* "$STATE_DIR"/.[!.]* "$STATE_DIR"/..?*; do
        if [ ! -e "$entry" ] && [ ! -L "$entry" ]; then
            continue
        fi
        base="$(basename -- "$entry")"
        if [ -L "$entry" ]; then
            die "state entry ${entry} is a symlink; refusing to touch state"
        fi
        if [ ! -f "$entry" ]; then
            die "state entry ${entry} is not a regular file; refusing to touch state"
        fi
        case "$base" in
            manifest | prior.rules) ;;
            .aa-usb-policy.*) ;;
            *) die "unexpected state entry ${entry}; refusing to touch state" ;;
        esac
    done
}

# state_validate - strictly parse and validate the saved-state manifest
# (findings F4). Enforces: exact key set with no duplicates and no unknown
# keys, strict per-key formats, and the cross-field invariants
#   prior_existed=1 <=> prior.rules exists and its sha256 == prior_sha256
#   prior_existed=0 <=> prior.rules is absent and prior_sha256 == "-"
# so a flipped prior_existed can never cause the original backup to be
# deleted or a backup to be silently re-based. Sets PRIOR_EXISTED, PRIOR_SHA,
# INSTALLED_SHA, INSTALLED_AT and PHASE (the transaction phase; optional key,
# default "installed" for manifests written before phases existed).
# Callers must run state_inventory_check first.
state_validate() {
    local line key val seen="" n=0
    PRIOR_EXISTED="" PRIOR_SHA="" INSTALLED_SHA="" INSTALLED_AT="" PHASE="installed"
    local schema="" rules_name=""
    while IFS= read -r line || [ -n "$line" ]; do
        n=$((n + 1))
        if [ -z "$line" ]; then
            continue
        fi
        case "$line" in
            *"="*)
                key="${line%%=*}"
                val="${line#*=}"
                ;;
            *)
                die "manifest line ${n} is not key=value; refusing to touch state"
                ;;
        esac
        case " $seen " in
            *" $key "*) die "manifest has duplicate key '${key}'; refusing to touch state" ;;
        esac
        seen="$seen $key"
        case "$key" in
            schema) schema="$val" ;;
            rules_name) rules_name="$val" ;;
            prior_existed) PRIOR_EXISTED="$val" ;;
            prior_sha256) PRIOR_SHA="$val" ;;
            installed_sha256) INSTALLED_SHA="$val" ;;
            installed_at_utc) INSTALLED_AT="$val" ;;
            phase) PHASE="$val" ;;
            *) die "manifest has unknown key '${key}'; refusing to touch state" ;;
        esac
    done <"$MANIFEST"
    for key in schema rules_name prior_existed prior_sha256 installed_sha256 installed_at_utc; do
        case " $seen " in
            *" $key "*) ;;
            *) die "manifest lacks ${key}; refusing to touch state" ;;
        esac
    done
    case "$PHASE" in
        installing | installed | restoring | restored) ;;
        *) die "manifest phase must be installing/installed/restoring/restored (got '${PHASE}'); refusing to touch state" ;;
    esac
    [ "$schema" = "$STATE_SCHEMA" ] \
        || die "unrecognized manifest schema '${schema}'; refusing to touch state"
    [ "$rules_name" = "$RULES_NAME" ] \
        || die "manifest is for a different rules file; refusing to touch state"
    case "$PRIOR_EXISTED" in
        0 | 1) ;;
        *) die "manifest prior_existed must be 0 or 1 (got '${PRIOR_EXISTED}'); refusing to touch state" ;;
    esac
    if ! printf '%s' "$PRIOR_SHA" | grep -Eq '^(-|[0-9a-f]{64})$'; then
        die "manifest prior_sha256 missing or not a sha256/sentinel (got '${PRIOR_SHA}'); refusing to touch state"
    fi
    printf '%s' "$INSTALLED_SHA" | grep -Eq '^[0-9a-f]{64}$' \
        || die "manifest installed_sha256 is not a sha256 (got '${INSTALLED_SHA}'); refusing to touch state"
    printf '%s' "$INSTALLED_AT" | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$' \
        || die "manifest installed_at_utc is not an ISO-8601 UTC stamp (got '${INSTALLED_AT}'); refusing to touch state"
    if [ "$PRIOR_EXISTED" = "1" ]; then
        printf '%s' "$PRIOR_SHA" | grep -Eq '^[0-9a-f]{64}$' \
            || die "manifest prior_existed=1 but prior_sha256 is not a sha256 (got '${PRIOR_SHA}'); refusing to touch state"
        [ -f "$PRIOR" ] || die "manifest records a prior rules file but ${PRIOR} is missing; refusing to touch state"
        [ ! -L "$PRIOR" ] || die "backup ${PRIOR} is a symlink; refusing to trust it as the recorded prior state"
        local actual_prior_sha
        actual_prior_sha="$(sha_of "$PRIOR")"
        [ "$actual_prior_sha" = "$PRIOR_SHA" ] \
            || die "prior backup hash mismatch (recorded ${PRIOR_SHA}, got ${actual_prior_sha}); refusing to touch state"
    else
        if [ -e "$PRIOR" ] || [ -L "$PRIOR" ]; then
            die "manifest records no prior rules file but ${PRIOR} exists; refusing to touch state (would discard the original backup)"
        fi
        [ "$PRIOR_SHA" = "-" ] \
            || die "manifest prior_existed=0 but prior_sha256 is '${PRIOR_SHA}' (want '-'); refusing to touch state"
    fi
}

# manifest_write PHASE - atomically publish the manifest from the current
# PRIOR_EXISTED/PRIOR_SHA/INSTALLED_SHA/INSTALLED_AT values and sync it to
# disk. Callers record the transaction phase BEFORE every destructive change
# (finding F5) so an interrupted run is always recoverable from the manifest.
manifest_write() {
    {
        printf 'schema=%s\n' "$STATE_SCHEMA"
        printf 'rules_name=%s\n' "$RULES_NAME"
        printf 'prior_existed=%s\n' "$PRIOR_EXISTED"
        printf 'prior_sha256=%s\n' "$PRIOR_SHA"
        printf 'installed_sha256=%s\n' "$INSTALLED_SHA"
        printf 'installed_at_utc=%s\n' "$INSTALLED_AT"
        printf 'phase=%s\n' "$1"
    } | publish_stdin "$MANIFEST"
    PHASE="$1"
    if command -v sync >/dev/null 2>&1; then
        sync -f "$MANIFEST" 2>/dev/null || sync 2>/dev/null || true
    fi
}

# Lock the stable /var/lib ancestor, not a directory replaced or removed by
# the transaction. Otherwise a waiting process can lock a retired inode.
lock_state() {
    if ! command -v flock >/dev/null 2>&1; then
        die "flock is required to serialize install/rollback; refusing to run unlocked"
    fi
    local lock_dir="${ROOT}/var/lib"
    exec 9<"$lock_dir" || die "cannot open ${lock_dir} for locking"
    flock -n 9 || die "another install/rollback is in progress (${STATE_DIR} is locked); refusing to interleave"
}
