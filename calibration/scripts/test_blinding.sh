#!/usr/bin/env bash
# Turing calibration (CAL-0 / EXP-001): blinding self-test (make test-turing-cal-blinding).
#
# Plants a random canary file inside the REAL sealed root, then checks, through
# the real wrappers:
#   candidate jail: cannot read the canary, cannot list the sealed root, cannot
#     find the canary by name anywhere, cannot reach it through /proc/*/root,
#     cannot create a user namespace or mount, has no network; CAN read dev data
#     and CAN write its working directory;
#   candidate wrapper: refuses to bind a directory that contains the sealed root;
#   evaluator jail: CAN read the canary, cannot write the frozen tree, CAN write
#     its output directory, has no network.
# The canary is removed on exit. Exit 0 = all checks pass.
set -uo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tc_jail_lib.sh
. "$here/tc_jail_lib.sh"
C="$here/candidate_env.sh"
V="$here/evaluator_env.sh"
dev="${TC_TEST_DEV:-$HOME/aien-data/crumbline}"
devfile="$(find "$dev" -name trace.ctr -type f | head -1)"
[ -n "$devfile" ] || { echo "no development trace.ctr under $dev" >&2; exit 2; }

pass=0 fail=0
ok() { echo "PASS $*"; pass=$((pass + 1)); }
bad() { echo "FAIL $*"; fail=$((fail + 1)); }
expect_fail() { local what="$1"; shift; if "$@" >/dev/null 2>&1; then bad "$what (succeeded)"; else ok "$what"; fi; }
expect_ok() { local what="$1"; shift; if "$@" >/dev/null 2>&1; then ok "$what"; else bad "$what (failed)"; fi; }

created_root=0
[ -d "$TC_SEALED_ROOT" ] || { mkdir -p "$TC_SEALED_ROOT" && chmod 0700 "$TC_SEALED_ROOT" && created_root=1; }
tag="blinding-canary-$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')"
canary_dir="$TC_SEALED_ROOT/.$tag"
mkdir -m 0700 "$canary_dir"
secret="$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')"
printf '%s\n' "$secret" >"$canary_dir/$tag.bin"
scratch="$(mktemp -d)"
frozen="$scratch/frozen"
evout="$scratch/eval-out"
work="$scratch/candidate-work"
mkdir -p "$frozen" "$work"
echo frozen >"$frozen/artifact"
cleanup() {
    rm -rf "$canary_dir" "$scratch"
    [ "$created_root" = 1 ] && rmdir "$TC_SEALED_ROOT" 2>/dev/null
    true
}
trap cleanup EXIT

cand() { "$C" --workdir "$work" --dev "$dev" -- "$@"; }
eval_() { "$V" --frozen "$frozen" --sealed "$canary_dir" --out "$evout" -- "$@"; }

echo "== candidate environment"
expect_fail "candidate cannot read the canary" cand cat "$canary_dir/$tag.bin"
expect_fail "candidate cannot list the sealed root" cand ls "$TC_SEALED_ROOT"
out="$(cand sh -c "find / -name '*$tag*' 2>/dev/null" || true)"
[ -z "$out" ] && ok "candidate finds no canary by name anywhere" || bad "candidate found: $out"
out="$(cand sh -c "cat /proc/*/root$canary_dir/$tag.bin 2>/dev/null" || true)"
[ "$out" != "$secret" ] && ok "candidate cannot reach the canary via /proc/*/root" || bad "canary via /proc"
expect_fail "candidate cannot create a user namespace" cand unshare --user --map-root-user true
expect_fail "candidate cannot mount" cand mount -t tmpfs none /tmp
expect_fail "candidate has no network" cand bash -c 'exec 3<>/dev/tcp/1.1.1.1/53'
expect_ok "candidate can read development data" cand head -c 247 "$devfile"
expect_ok "candidate can write its working directory" cand sh -c "echo x > '$work/probe'"
expect_fail "candidate wrapper refuses a bind containing the sealed root" \
    "$C" --workdir "$work" --dev "$(dirname "$TC_SEALED_ROOT")" -- true

echo "== evaluator environment"
out="$(eval_ cat "$canary_dir/$tag.bin" 2>/dev/null || true)"
[ "$out" = "$secret" ] && ok "evaluator can read sealed data" || bad "evaluator cannot read sealed data"
expect_fail "evaluator cannot modify the frozen tree" eval_ sh -c "echo y > '$frozen/artifact'"
expect_fail "evaluator cannot write sealed data" eval_ sh -c "echo y > '$canary_dir/new'"
expect_ok "evaluator can write its output directory" eval_ sh -c "echo z > '$evout/result'"
expect_fail "evaluator has no network" eval_ bash -c 'exec 3<>/dev/tcp/1.1.1.1/53'
expect_fail "evaluator cannot see development data" eval_ head -c 1 "$devfile"
expect_fail "evaluator wrapper refuses --out inside the frozen tree" \
    "$V" --frozen "$frozen" --sealed "$canary_dir" --out "$frozen/o" -- true

echo "blinding self-test: $pass passed, $fail failed"
[ "$fail" = 0 ]
