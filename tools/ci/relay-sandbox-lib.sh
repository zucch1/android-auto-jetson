# SPDX-License-Identifier: GPL-3.0-or-later
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
#   RELAY_SB_ENV  array of KEY=VALUE pairs; this is the COMPLETE environment of the sandbox
#   RELAY_SB_HOME in-sandbox HOME (a fresh empty tmpfs; default /home/aa-relay)
#   RELAY_SB_PATH in-sandbox PATH (default /usr/local/bin:/usr/bin:/bin)
# Optional: AA_RELAY_EXTRA_RO (colon-separated host paths to expose read-only, e.g. /opt/toolchain),
#           AA_RELAY_STEP_TIMEOUT (seconds per sandboxed command; default 3600).

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

# relay_sandbox_run <backend> <workdir> <command...>   (stdout/stderr pass through)
relay_sandbox_run() {
    local backend=$1 workdir=$2
    shift 2
    local home=${RELAY_SB_HOME:-/home/aa-relay}
    local sb_path=${RELAY_SB_PATH:-/usr/local/bin:/usr/bin:/bin}
    local timeout_s=${AA_RELAY_STEP_TIMEOUT:-3600}
    local -a ro=("${RELAY_SB_RO[@]}") rw=("${RELAY_SB_RW[@]}") envs=("HOME=$home" "PATH=$sb_path" "GIT_CONFIG_NOSYSTEM=1" "${RELAY_SB_ENV[@]}")
    local -a extra=() argv
    local item d
    if [ -n "${AA_RELAY_EXTRA_RO:-}" ]; then
        IFS=: read -r -a extra <<<"$AA_RELAY_EXTRA_RO"
        ro+=("${extra[@]}")
    fi
    case "$backend" in
        bwrap)
            argv=(bwrap --unshare-all --die-with-parent --new-session --clearenv
                --ro-bind /usr /usr --proc /proc --dev /dev --tmpfs /tmp --tmpfs "$home")
            for d in bin sbin lib lib32 lib64; do
                if [ -L "/$d" ]; then
                    argv+=(--symlink "$(readlink "/$d")" "/$d")
                elif [ -d "/$d" ]; then
                    argv+=(--ro-bind "/$d" "/$d")
                fi
            done
            for d in alternatives ld.so.cache ld.so.conf ld.so.conf.d localtime ssl; do
                argv+=(--ro-bind-try "/etc/$d" "/etc/$d")
            done
            for item in "${ro[@]}"; do argv+=(--ro-bind "$item" "$item"); done
            for item in "${rw[@]}"; do argv+=(--bind "$item" "$item"); done
            for item in "${envs[@]}"; do argv+=(--setenv "${item%%=*}" "${item#*=}"); done
            argv+=(--chdir "$workdir" -- "$@")
            # env -i: the bwrap process itself holds none of the caller's variables either.
            timeout --kill-after=30 "$timeout_s" env -i PATH=/usr/bin:/bin "${argv[@]}"
            ;;
        podman | docker)
            argv=("$backend" run --rm --network none --read-only --cap-drop ALL
                --security-opt no-new-privileges --pids-limit 4096
                --tmpfs "/tmp:rw,exec,mode=1777" --tmpfs "$home:rw,mode=1777" -w "$workdir")
            if [ "$backend" = podman ]; then
                argv+=(--userns keep-id)
            else
                argv+=(--user "$(id -u):$(id -g)")
            fi
            for item in "${ro[@]}"; do argv+=(-v "$item:$item:ro"); done
            for item in "${rw[@]}"; do argv+=(-v "$item:$item:rw"); done
            for item in "${envs[@]}"; do argv+=(-e "$item"); done
            argv+=("$AA_RELAY_IMAGE" "$@")
            # Container env is not inherited from the host; only the -e pairs above exist inside.
            timeout --kill-after=30 "$timeout_s" "${argv[@]}"
            ;;
        *)
            echo "error: unknown sandbox backend '$backend'" >&2
            return 70
            ;;
    esac
}
