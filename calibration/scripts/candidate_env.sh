#!/usr/bin/env bash
# Turing calibration (CAL-0 / EXP-001): Candidate Environment.
#
# usage: candidate_env.sh [--workdir DIR] [--dev DIR]... [--ro DIR]... [--net] -- COMMAND [ARG...]
#
# Runs COMMAND in a bubblewrap jail that can see ONLY:
#   * the system (/usr, /etc read-only), an empty /tmp and an empty HOME;
#   * the candidate working directory (default: the current git worktree), read-write;
#   * the git common directory of that worktree, read-only (so `git log/show` work);
#   * development data (default: ~/aien-data/crumbline), read-only;
#   * extra read-only paths given with --ro (for example the public profile).
# It can NOT see the sealed root ($TC_SEALED_ROOT, default ~/aien-data/turing-cal/sealed)
# or the evaluator output root ($TC_EVAL_ROOT): those paths do not exist in the jail.
# Any requested bind that overlaps them is refused before the jail starts.
# Network is off unless --net is given (the evaluator is never reachable either way:
# it runs offline in its own jail and exposes no service).
#
# Exit code: COMMAND's exit code, or 3 when the jail refuses to start.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tc_jail_lib.sh
. "$here/tc_jail_lib.sh"

workdir=""
devs=()
ros=()
net=0
while [ $# -gt 0 ]; do
    case "$1" in
    --workdir) workdir="$2"; shift 2 ;;
    --dev) devs+=("$2"); shift 2 ;;
    --ro) ros+=("$2"); shift 2 ;;
    --net) net=1; shift ;;
    --) shift; break ;;
    *) tc_die "unknown option '$1' (use -- before the command)" ;;
    esac
done
[ $# -gt 0 ] || tc_die "no command given"
[ -n "$workdir" ] || workdir="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
[ ${#devs[@]} -gt 0 ] || devs=("$HOME/aien-data/crumbline")

tc_have_bwrap
hidden=("$TC_SEALED_ROOT" "$TC_EVAL_ROOT")
tc_refuse_hidden "$workdir" workdir "${hidden[@]}"
for d in "${devs[@]}"; do tc_refuse_hidden "$d" "dev data" "${hidden[@]}"; done
for d in "${ros[@]+"${ros[@]}"}"; do tc_refuse_hidden "$d" "read-only path" "${hidden[@]}"; done

tc_base_args
if [ "$net" = 1 ]; then
    TC_ARGS+=(--share-net)
    if [ -d /run/systemd/resolve ]; then tc_ro /run/systemd/resolve; fi
fi
for d in "${devs[@]}"; do tc_ro "$d"; done
for d in "${ros[@]+"${ros[@]}"}"; do tc_ro "$d"; done
gitcommon="$(git -C "$workdir" rev-parse --path-format=absolute --git-common-dir 2>/dev/null || true)"
if [ -n "$gitcommon" ] && [ -d "$gitcommon" ]; then
    tc_refuse_hidden "$gitcommon" "git dir" "${hidden[@]}"
    tc_ro "$gitcommon"
fi
tc_rw "$workdir"
TC_ARGS+=(--setenv TC_ENV candidate --chdir "$(tc_real "$workdir")")
exec "$TC_BWRAP" "${TC_ARGS[@]}" -- "$@"
