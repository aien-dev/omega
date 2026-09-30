# Turing calibration (CAL-0 / EXP-001): shared bubblewrap jail helpers.
# Sourced by candidate_env.sh, evaluator_env.sh, generate_sealed_data.sh and
# test_blinding.sh. calibration/docs/BLINDING_PROTOCOL.md explains the design.
#
# The jails are ALLOWLISTS: the new mount namespace starts empty and only the
# paths bound here exist inside it. A path that is not bound (for example the
# sealed root inside the Candidate Environment) does not exist for any process
# in the jail. Enforcement is the Linux kernel (mount + user + pid + net
# namespaces via /usr/bin/bwrap, no sudo). It covers every process launched
# through a wrapper; it does not cover a process of the same uid that is started
# outside a wrapper (see BLINDING_PROTOCOL.md section 5 for the uid upgrade).

TC_SEALED_ROOT="${TC_SEALED_ROOT:-$HOME/aien-data/turing-cal/sealed}"
TC_EVAL_ROOT="${TC_EVAL_ROOT:-$HOME/aien-data/turing-cal/eval}"
TC_BWRAP="${TC_BWRAP:-/usr/bin/bwrap}"

tc_die() { echo "tc-jail: $*" >&2; exit 3; }

tc_real() { realpath -m -- "$1"; }

# tc_overlaps A B: true when A equals B, or one contains the other.
tc_overlaps() {
    local a b
    a="$(tc_real "$1")"
    b="$(tc_real "$2")"
    [ "$a" = "$b" ] && return 0
    case "$a/" in "$b"/*) return 0 ;; esac
    case "$b/" in "$a"/*) return 0 ;; esac
    return 1
}

# tc_refuse_hidden PATH LABEL: refuse to bind a path that overlaps a hidden root.
tc_refuse_hidden() {
    local p="$1" label="$2" h
    shift 2
    for h in "$@"; do
        if tc_overlaps "$p" "$h"; then
            tc_die "refusing to bind $label '$p': it overlaps hidden path '$h'"
        fi
    done
}

# Base system: read-only /usr and /etc, merged-usr symlinks, private /proc,
# minimal /dev, empty /tmp and HOME, no user namespaces inside, all namespaces
# unshared (network included), killed with the parent, new session (no tty
# injection). Output: one argument per line in TC_ARGS (bash array).
tc_base_args() {
    TC_ARGS=(
        --unshare-all --unshare-user --disable-userns
        --die-with-parent --new-session
        --ro-bind /usr /usr
        --ro-bind /etc /etc
        --proc /proc
        --dev /dev
        --tmpfs /tmp
        --tmpfs /home
        --dir "$HOME"
        --setenv HOME "$HOME"
        --setenv PATH /usr/local/bin:/usr/bin:/bin
        --unsetenv SSH_AUTH_SOCK
        --unsetenv DBUS_SESSION_BUS_ADDRESS
    )
    local l
    for l in bin sbin lib lib32 lib64 libx32; do
        if [ -L "/$l" ]; then
            TC_ARGS+=(--symlink "$(readlink "/$l")" "/$l")
        elif [ -d "/$l" ]; then
            TC_ARGS+=(--ro-bind "/$l" "/$l")
        fi
    done
}

tc_ro() { [ -e "$1" ] || tc_die "missing path '$1'"; TC_ARGS+=(--ro-bind "$(tc_real "$1")" "$(tc_real "$1")"); }
tc_rw() { [ -e "$1" ] || tc_die "missing path '$1'"; TC_ARGS+=(--bind "$(tc_real "$1")" "$(tc_real "$1")"); }

tc_have_bwrap() {
    [ -x "$TC_BWRAP" ] || tc_die "bubblewrap not found at $TC_BWRAP"
    "$TC_BWRAP" --unshare-all --unshare-user --disable-userns --ro-bind /usr /usr \
        --symlink usr/bin /bin --symlink usr/lib /lib /usr/bin/true 2>/dev/null ||
        tc_die "bubblewrap cannot create an unprivileged sandbox on this host"
}
