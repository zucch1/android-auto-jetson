# SPDX-License-Identifier: GPL-3.0-or-later
# shellcheck shell=bash
# relay-sandbox-lib.sh — sourced by arm64-cross-relay.sh (and tests/ci/relay_sandbox_test.py).
#
# Runs ONE command inside an isolated sandbox. The untrusted PR tree is built here, so the
# sandbox must not see the owner's $HOME (gh token, SSH keys), SSH agent, environment,
# network, or any writable view of the private sysroot. Fail closed: if no usable backend
# exists, relay_sandbox_select returns non-zero and the caller must stop; there is no
# unsandboxed fallback.
#
# Backends (auto-detect order: podman, bwrap, docker; override with --sandbox / AA_RELAY_SANDBOX):
#   podman  rootless container; needs AA_RELAY_IMAGE (cmake, ninja, aarch64 g++, python3)
#   bwrap   bubblewrap over the host's read-only /usr (no image needed)
#   docker  container via the docker daemon (root-equivalent; needs AA_RELAY_IMAGE)
#
# Callers set before relay_sandbox_run:
#   RELAY_SB_RW   array of paths mounted read-write at the same absolute path
#   RELAY_SB_RO   array of paths mounted read-only at the same absolute path
#   RELAY_SB_PROTECT        optional: extra paths no writable mount may equal, contain or sit inside
#   RELAY_SB_PROTECT_COVER  optional: paths no writable mount may equal or contain, but may sit
#                           inside (the owner's \$HOME: a dedicated cache under it is fine)
#   RELAY_SB_ENV  array of KEY=VALUE pairs; this is the COMPLETE environment of the sandbox
#   RELAY_SB_HOME in-sandbox HOME (a fresh empty tmpfs; default /home/aa-relay)
#   RELAY_SB_PATH in-sandbox PATH (default /usr/local/bin:/usr/bin:/bin)
# Optional: AA_RELAY_EXTRA_RO (colon-separated host paths to expose read-only, e.g. /opt/toolchain),
#           AA_RELAY_STEP_TIMEOUT (seconds per sandboxed command; default 3600),
#           AA_RELAY_KILL_AFTER (seconds between TERM and KILL of the launcher; default 30),
#           AA_RELAY_CONTAINER_PREFIX (container name prefix; default aa-relay).
#
# Hardening contract (review of PR #92):
#   * Every file descriptor above 2 is closed before the sandbox (or the container client) is
#     exec'd, and stdin is /dev/null: `env -i` clears variables but not fds, and a leaked
#     directory fd (e.g. the owner's HOME) would let the sandbox openat() its way to ~/.config/gh.
#   * All writable and protected paths are canonicalised (realpath); the run is REFUSED if a
#     writable path equals, contains or is contained by a protected one (read-only inputs and the
#     sandbox's own /usr, /etc, ...). Writable mounts are emitted first and read-only last, so a
#     read-only mount always wins.
#   * Containers get a known random name; on timeout, error, INT/TERM or normal exit they are
#     `kill`ed and `rm -f`ed and verified GONE before the caller hashes logs or posts a status.
#     If that cannot be confirmed relay_sandbox_run returns 71 and the caller must stop.

relay_sandbox_probe() {
    case "$1" in
        bwrap)
            command -v bwrap >/dev/null 2>&1 &&
                bwrap --unshare-all --ro-bind / / true >/dev/null 2>&1 ;;
        podman | docker)
            [ -n "${AA_RELAY_IMAGE:-}" ] && command -v "$1" >/dev/null 2>&1 &&
                "$1" image inspect "$AA_RELAY_IMAGE" >/dev/null 2>&1 ;;
        *) return 1 ;;
    esac
}

# relay_sandbox_select [requested|auto] -> prints the backend name or a message on stderr.
relay_sandbox_select() {
    local requested=${1:-auto} candidate
    if [ "$requested" != auto ]; then
        if relay_sandbox_probe "$requested"; then
            echo "$requested"
            return 0
        fi
        echo "error: requested sandbox backend '$requested' is not usable here" >&2
        echo "       (bwrap needs unprivileged user namespaces; podman/docker need AA_RELAY_IMAGE to be a local image)" >&2
        return 1
    fi
    for candidate in podman bwrap docker; do
        if relay_sandbox_probe "$candidate"; then
            echo "$candidate"
            return 0
        fi
    done
    {
        echo "error: no usable sandbox backend; refusing to build the PR tree unsandboxed."
        echo "       Install bubblewrap (bwrap), or podman/docker plus a local image named in AA_RELAY_IMAGE."
    } >&2
    return 1
}

# Python is the trusted-side launcher helper: close every fd > 2, then exec the real command.
# (bwrap and the container clients have no close-all option, and bash cannot enumerate fds
# without opening one.) stdin is replaced by /dev/null by the caller.
RELAY_CLOSER_PY='
import os, sys
for name in os.listdir("/proc/self/fd"):
    fd = int(name)
    if fd > 2:
        try:
            os.close(fd)
        except OSError:
            pass
os.execvp(sys.argv[1], sys.argv[1:])
'

# relay_sandbox_overlaps A B: true if A equals, contains or is contained by B (canonical paths).
relay_sandbox_overlaps() {
    local a=${1%/} b=${2%/}
    [ -z "$a" ] || [ -z "$b" ] || [ "$a" = "$b" ] || [ "${a#"$b"/}" != "$a" ] || [ "${b#"$a"/}" != "$b" ]
}

# relay_sandbox_canonicalize: rewrite RELAY_SB_RO/RELAY_SB_RW (+ AA_RELAY_EXTRA_RO) to
# canonical absolute paths in RELAY_SB_CRO / RELAY_SB_CRW, then refuse any rw/ro overlap.
relay_sandbox_canonicalize() {
    local item c d
    local -a extra=()
    RELAY_SB_CRO=()
    RELAY_SB_CRW=()
    if [ -n "${AA_RELAY_EXTRA_RO:-}" ]; then
        IFS=: read -r -a extra <<<"$AA_RELAY_EXTRA_RO"
    fi
    for item in "${RELAY_SB_RO[@]}" "${extra[@]}"; do
        [ -n "$item" ] || continue
        c=$(realpath -m -- "$item") || return 70
        RELAY_SB_CRO+=("$c")
    done
    for item in "${RELAY_SB_RW[@]}"; do
        c=$(realpath -m -- "$item") || return 70
        RELAY_SB_CRW+=("$c")
    done
    # Protected paths = caller's read-only inputs + the sandbox's own system mounts + the
    # caller's RELAY_SB_PROTECT (trusted scratch such as logs, base checkout, empty HOME, the
    # repo's git dirs): a writable path may not equal, contain or sit inside any of them.
    local -a protected=("${RELAY_SB_CRO[@]}" /usr /bin /sbin /lib /lib32 /lib64 /etc /proc /dev)
    for item in ${RELAY_SB_PROTECT[@]+"${RELAY_SB_PROTECT[@]}"}; do
        [ -n "$item" ] || continue
        c=$(realpath -m -- "$item") || return 70
        protected+=("$c")
    done
    # RELAY_SB_PROTECT_COVER: paths a writable mount may sit inside but never equal or contain
    # (the owner's $HOME: a writable mount of it or a parent would expose it).
    local -a cover=()
    for item in ${RELAY_SB_PROTECT_COVER[@]+"${RELAY_SB_PROTECT_COVER[@]}"}; do
        [ -n "$item" ] || continue
        c=$(realpath -m -- "$item") || return 70
        cover+=("$c")
    done
    for c in "${RELAY_SB_CRW[@]}"; do
        if [ "$c" = / ]; then
            echo "error: refusing to run: the filesystem root cannot be a writable mount" >&2
            return 65
        fi
        for d in "${protected[@]}"; do
            if relay_sandbox_overlaps "$c" "$d"; then
                echo "error: refusing to run: writable path '$c' overlaps protected path '$d'" >&2
                echo "       (a writable mount must never equal, contain or sit inside a read-only input)" >&2
                return 65
            fi
        done
        for d in ${cover[@]+"${cover[@]}"}; do
            if [ "$c" = "$d" ] || [ "${d#"${c%/}"/}" != "$d" ]; then
                echo "error: refusing to run: writable path '$c' equals or contains protected path '$d'" >&2
                echo "       (it would expose the owner's HOME inside the sandbox; use a fresh dedicated directory)" >&2
                return 65
            fi
        done
    done
    return 0
}

# Marker the relay itself creates in a directory it was told to use as AA_SYSROOT_CACHE or
# AA_RELAY_SCRATCH. A non-empty directory without it (~/.ssh, ~/.config, a project) is never
# adopted: the sandbox gets a writable view of the cache, so it must be dedicated.
RELAY_DEDICATED_MARKER=.aa-relay-dedicated

# relay_dedicated_check <path> <label>: 0 when <path> is nonexistent, an empty directory, or a
# directory holding the marker (a regular file); 65 otherwise. Creates nothing.
relay_dedicated_check() {
    local dir=$1 label=$2
    [ -e "$dir" ] || [ -L "$dir" ] || return 0
    if [ ! -d "$dir" ]; then
        echo "error: refusing to run: $label '$dir' exists and is not a directory" >&2
        return 65
    fi
    if [ -f "$dir/$RELAY_DEDICATED_MARKER" ] && [ ! -L "$dir/$RELAY_DEDICATED_MARKER" ]; then
        return 0
    fi
    if [ -z "$(find "$dir" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
        return 0
    fi
    echo "error: refusing to run: $label '$dir' is a non-empty directory the relay did not create" >&2
    echo "       (no $RELAY_DEDICATED_MARKER marker; use a nonexistent or empty directory, never one that holds your files)" >&2
    return 65
}

# relay_dedicated_owner_check <path> <label>: an existing directory must belong to the current
# user and not be group/world-writable (another user could swap its contents under the relay).
relay_dedicated_owner_check() {
    local dir=$1 label=$2 owner mode
    [ -d "$dir" ] || return 0
    owner=$(stat -c %u -- "$dir") && mode=$(stat -c %a -- "$dir") || return 70
    if [ "$owner" != "$(id -u)" ]; then
        echo "error: refusing to run: $label '$dir' is not owned by the current user" >&2
        return 65
    fi
    if [ $((8#$mode & 8#022)) -ne 0 ]; then
        echo "error: refusing to run: $label '$dir' is group- or world-writable (mode $mode)" >&2
        return 65
    fi
    return 0
}

# relay_dedicated_claim <path>: create the directory (0700) and its marker.
relay_dedicated_claim() {
    local dir=$1
    if [ ! -d "$dir" ]; then
        mkdir -p -- "$dir" && chmod 0700 -- "$dir" || return 70
    fi
    [ -e "$dir/$RELAY_DEDICATED_MARKER" ] || : >"$dir/$RELAY_DEDICATED_MARKER" || return 70
}

# relay_sandbox_build_argv <backend> <workdir> <command...>: fills RELAY_SB_ARGV (client or bwrap
# command line) and RELAY_SB_NAME (container name, containers only). Does not run anything.
relay_sandbox_build_argv() {
    local backend=$1 workdir=$2
    shift 2
    local home=${RELAY_SB_HOME:-/home/aa-relay}
    local sb_path=${RELAY_SB_PATH:-/usr/local/bin:/usr/bin:/bin}
    local -a envs=("HOME=$home" "PATH=$sb_path" "GIT_CONFIG_NOSYSTEM=1" "${RELAY_SB_ENV[@]}")
    local item d rc=0
    relay_sandbox_canonicalize || rc=$?
    [ "$rc" -eq 0 ] || return "$rc"
    workdir=$(realpath -m -- "$workdir")
    RELAY_SB_ARGV=()
    RELAY_SB_NAME=
    case "$backend" in
        bwrap)
            RELAY_SB_ARGV=(bwrap --unshare-all --die-with-parent --new-session --clearenv
                --ro-bind /usr /usr --proc /proc --dev /dev --tmpfs /tmp --tmpfs "$home")
            for d in bin sbin lib lib32 lib64; do
                if [ -L "/$d" ]; then
                    RELAY_SB_ARGV+=(--symlink "$(readlink "/$d")" "/$d")
                elif [ -d "/$d" ]; then
                    RELAY_SB_ARGV+=(--ro-bind "/$d" "/$d")
                fi
            done
            for d in alternatives ld.so.cache ld.so.conf ld.so.conf.d localtime ssl; do
                RELAY_SB_ARGV+=(--ro-bind-try "/etc/$d" "/etc/$d")
            done
            # Writable first, protected read-only LAST: later mounts win, so ro can never be masked.
            for item in "${RELAY_SB_CRW[@]}"; do RELAY_SB_ARGV+=(--bind "$item" "$item"); done
            for item in "${RELAY_SB_CRO[@]}"; do RELAY_SB_ARGV+=(--ro-bind "$item" "$item"); done
            for item in "${envs[@]}"; do RELAY_SB_ARGV+=(--setenv "${item%%=*}" "${item#*=}"); done
            RELAY_SB_ARGV+=(--chdir "$workdir" -- "$@")
            ;;
        podman | docker)
            RELAY_SB_NAME="${AA_RELAY_CONTAINER_PREFIX:-aa-relay}-$(od -An -N12 -tx1 /dev/urandom | tr -d ' \n')"
            RELAY_SB_ARGV=("$backend" run --rm --name "$RELAY_SB_NAME" --network none --read-only --cap-drop ALL
                --security-opt no-new-privileges --pids-limit 4096
                --tmpfs "/tmp:rw,exec,mode=1777" --tmpfs "$home:rw,mode=1777" -w "$workdir")
            if [ "$backend" = podman ]; then
                RELAY_SB_ARGV+=(--userns keep-id)
            else
                RELAY_SB_ARGV+=(--user "$(id -u):$(id -g)")
            fi
            for item in "${RELAY_SB_CRW[@]}"; do RELAY_SB_ARGV+=(-v "$item:$item:rw"); done
            for item in "${RELAY_SB_CRO[@]}"; do RELAY_SB_ARGV+=(-v "$item:$item:ro"); done
            for item in "${envs[@]}"; do RELAY_SB_ARGV+=(-e "$item"); done
            RELAY_SB_ARGV+=("$AA_RELAY_IMAGE" "$@")
            ;;
        *)
            echo "error: unknown sandbox backend '$backend'" >&2
            return 70
            ;;
    esac
}

# Container bookkeeping for the active run (read by relay_sandbox_reap_active from EXIT/INT/TERM traps).
RELAY_SB_ACTIVE_BACKEND=
RELAY_SB_ACTIVE_NAME=
RELAY_SB_ACTIVE_PID=

# relay_container_gone <backend> <name>: true only if the daemon answered and no container
# of that name exists (a daemon that is down must NOT look like "gone").
relay_container_gone() {
    local out
    out=$("$1" ps -a -q --no-trunc --filter "name=^/?$2\$" 2>/dev/null) || return 1
    [ -z "$out" ]
}

# relay_sandbox_reap_active: stop whatever sandbox run is active. For containers: kill, rm -f,
# verify gone. Returns 0 when nothing is left running, 71 when that cannot be confirmed.
relay_sandbox_reap_active() {
    [ -n "$RELAY_SB_ACTIVE_BACKEND" ] || return 0
    if [ -n "$RELAY_SB_ACTIVE_PID" ]; then
        kill -KILL "$RELAY_SB_ACTIVE_PID" 2>/dev/null || true
    fi
    if [ -n "$RELAY_SB_ACTIVE_NAME" ]; then
        for _ in 1 2 3 4 5; do
            "$RELAY_SB_ACTIVE_BACKEND" kill "$RELAY_SB_ACTIVE_NAME" >/dev/null 2>&1 || true
            "$RELAY_SB_ACTIVE_BACKEND" rm -f "$RELAY_SB_ACTIVE_NAME" >/dev/null 2>&1 || true
            if relay_container_gone "$RELAY_SB_ACTIVE_BACKEND" "$RELAY_SB_ACTIVE_NAME"; then
                RELAY_SB_ACTIVE_BACKEND=
                RELAY_SB_ACTIVE_NAME=
                RELAY_SB_ACTIVE_PID=
                return 0
            fi
            sleep 1
        done
        echo "error: could not confirm that container $RELAY_SB_ACTIVE_NAME is gone; untrusted code may still be running" >&2
        return 71
    fi
    RELAY_SB_ACTIVE_BACKEND=
    RELAY_SB_ACTIVE_PID=
    return 0
}

# relay_sandbox_run <backend> <workdir> <command...>   (stdout/stderr pass through)
# Returns the sandboxed command's status; 71 = termination could not be confirmed (caller must
# not trust anything and must post nothing); 65 = refused because of overlapping mounts.
relay_sandbox_run() {
    local backend=$1 workdir=$2
    local timeout_s=${AA_RELAY_STEP_TIMEOUT:-3600} kill_after=${AA_RELAY_KILL_AFTER:-30} rc=0
    relay_sandbox_build_argv "$@" || return $?
    RELAY_SB_ACTIVE_BACKEND=$backend
    RELAY_SB_ACTIVE_NAME=$RELAY_SB_NAME
    # Backgrounded + `wait`, so INT/TERM traps in the caller run immediately instead of after the
    # sandbox ends. timeout only kills the CLIENT; relay_sandbox_reap_active ends the sandbox itself.
    # env -i also keeps the owner's variables out of bwrap; the container client keeps the owner's
    # env (it needs DOCKER_HOST etc.) but the container only gets the -e pairs.
    local py
    py=$(command -v python3) || { echo "error: python3 is required for the fd-closing launcher" >&2; return 70; }
    if [ "$backend" = bwrap ]; then
        timeout --kill-after="$kill_after" "$timeout_s" \
            /usr/bin/env -i PATH=/usr/bin:/bin "$py" -I -B -c "$RELAY_CLOSER_PY" "${RELAY_SB_ARGV[@]}" </dev/null &
    else
        timeout --kill-after="$kill_after" "$timeout_s" \
            "$py" -I -B -c "$RELAY_CLOSER_PY" "${RELAY_SB_ARGV[@]}" </dev/null &
    fi
    RELAY_SB_ACTIVE_PID=$!
    wait "$RELAY_SB_ACTIVE_PID" || rc=$?
    # A signal trap in the caller exits before this point; here the launcher has been reaped.
    RELAY_SB_ACTIVE_PID=
    # Whatever happened (normal exit, timeout, client killed), end and verify the sandbox now.
    relay_sandbox_reap_active || return 71
    return "$rc"
}
