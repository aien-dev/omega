#!/usr/bin/env bash
# Turing calibration (CAL-0 / EXP-001, EXP-001R): blinding self-test (make test-turing-cal-blinding).
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
# Then the overlap audit self-tests on synthetic planted ledgers (amended G6, burned EXP-001 seeds).
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


echo "== overlap audit, amended G6 (synthetic planted ledgers; no real sealed data is read)"
V6="$here/verify_holdout_separation.sh"
G="$here/generate_sealed_data.sh"
ovroot="$scratch/ov"
mkdir -p "$ovroot"
hx() { printf '%s' "$1" | sha256sum | cut -d' ' -f1; }
ov_repo="$ovroot/repo"
mkdir -p "$ov_repo/calibration/experiments/EXP-001" "$ov_repo/calibration/experiments/EXP-001R"
for e in EXP-001 EXP-001R; do printf '{\n  "status": "frozen",\n}\n' >"$ov_repo/calibration/experiments/$e/candidate_manifest.json"; done
git -C "$ov_repo" init -q 2>/dev/null
git -C "$ov_repo" -c user.name=t -c user.email=t@t add calibration
git -C "$ov_repo" -c user.name=t -c user.email=t@t commit -q -m synthetic
ov_commit="$(git -C "$ov_repo" rev-parse HEAD)"
ov_digest="$(hx digest)"
ov_seed() { local h; h="$(printf '%s' "turing.cal.sealed.v1|$ov_commit|$ov_digest|g$1|$2" | sha256sum | cut -c1-16)"
    printf '%d\n' "$((16#$(printf '%x' $((16#${h:0:1} & 7)))${h:1}))"; }
sleep 1
# ledger line: population crumb sealed stream
ll() { printf '{"population":"%s","crumb_digest":"%s","sealed_digest":"%s","trace_stream_digest":"%s"}\n' "$1" "$2" "$3" "$4"; }
# mk_side DIR LEDGERLINE... : DIR/control/{trace.ctr (empty CTR1), ledger.jsonl}
mk_side() { local d="$1"; shift; mkdir -p "$d/control"; : >"$d/control/trace.ctr"; printf '%s\n' "$@" >"$d/control/ledger.jsonl"; }
# scenario NAME EXPID DEV_POP DEV_SEALED SEALED_POP SEALED_SEALED WHERE(dev|burned) -> runs the audit, sets ov_rc, ov_out
scenario() {
    local name="$1" expid="$2" dpop="$3" dsd="$4" spop="$5" ssd="$6" where="$7" d="$ovroot/$1"
    local x; x="$(hx "crumb-$name")"
    rm -rf "$d"; mkdir -p "$d"
    mk_side "$d/dev/a" "$(ll Fundamental "$(hx other-dev-$name)" "$(hx dv-$name)" "$(hx ds-$name)")"
    mk_side "$d/burned/a" "$(ll Fundamental "$(hx other-burned-$name)" "$(hx bv-$name)" "$(hx bs-$name)")"
    if [ "$where" = dev ]; then
        mk_side "$d/dev/b" "$(ll "$dpop" "$x" "$(hx "$dsd")" "$(hx dstream-$name)")"
    else
        mk_side "$d/burned/b" "$(ll "$dpop" "$x" "$(hx "$dsd")" "$(hx bstream-$name)")"
    fi
    find "$d/dev" -name trace.ctr | LC_ALL=C sort >"$d/dev.txt"
    local s1 s2; s1="$(ov_seed 1 0)"; s2="$(ov_seed 2 0)"
    mk_side "$d/sealed/group-1/seed-$s1" "$(ll Fundamental "$(hx s1-$name)" "$(hx sv1-$name)" "$(hx ss1-$name)")"
    mk_side "$d/sealed/group-2/seed-$s2" "$(ll "$spop" "$x" "$(hx "$ssd")" "$(hx sstream-$name)")"
    {
        printf '{\n  "freeze_commit": "%s",\n  "profile_digest": "%s",\n  "seeds": [\n' "$ov_commit" "$ov_digest"
        printf '    {"group": 1, "index": 0, "seed": %s},\n    {"group": 2, "index": 0, "seed": %s}\n  ]\n}\n' "$s1" "$s2"
    } >"$d/sealed/seed_commitment.json"
    {
        echo '{"files": ['
        n=0
        for t in "$d/sealed/group-1/seed-$s1/control/trace.ctr" "$d/sealed/group-2/seed-$s2/control/trace.ctr"; do
            [ "$n" = 0 ] || echo ","
            printf '  {"path": "%s", "sha256": "%s", "bytes": 0, "records": 0, "kept": true}' "${t#"$d/sealed/"}" "$(sha256sum "$t" | cut -d' ' -f1)"
            n=$((n + 1))
        done
        echo; echo ']}'
    } >"$d/sealed/manifest.json"
    sha256sum "$d/sealed/seed_commitment.json" | cut -d' ' -f1 >"$d/sealed/COMPLETE"
    ov_out="$d/overlap_audit.json"
    EXP_ID="$expid" TC_EXP001_SEALED="$d/burned" "$V6" --sealed-dir "$d/sealed" --out "$ov_out" \
        --dev-list "$d/dev.txt" --repo "$ov_repo" >"$d/audit.log" 2>&1
    ov_rc=$?
}
g6line() { grep '"crumb_digest": {' "$ov_out"; }

scenario i-dev EXP-001R Ambiguous hidden-dev Ambiguous hidden-sealed dev
[ "$ov_rc" = 0 ] && grep -q '"result": "PASS"' "$ov_out" && g6line | grep -q '"pass": true, "detail": "1 shared, 1 exempt (Ambiguous, different hidden set)"' \
    && [ "$(grep -c '"population": "Ambiguous", "sealed_digest"' "$ov_out")" = 1 ] \
    && ok "(i) Ambiguous visible-only match, different sealed_digest: audit PASS, 1 exempt, listed with both sealed_digests" \
    || bad "(i) Ambiguous different-hidden match should PASS with 1 exempt (rc $ov_rc: $(tail -2 "$ovroot/i-dev/audit.log" | tr '\n' ' '))"
scenario i-burned EXP-001R Ambiguous hidden-dev Ambiguous hidden-sealed burned
[ "$ov_rc" = 0 ] && g6line | grep -q '1 shared, 1 exempt' \
    && ok "(i-b) same match against a burned EXP-001 sealed trace: PASS, 1 exempt" \
    || bad "(i-b) burned-side Ambiguous different-hidden match should PASS (rc $ov_rc)"
scenario ii EXP-001R Ambiguous same-hidden Ambiguous same-hidden dev
[ "$ov_rc" = 1 ] && g6line | grep -q '"pass": false' && grep -q '"result": "FAIL"' "$ov_out" \
    && ok "(ii) Ambiguous match with the same sealed_digest: audit FAIL at G6" \
    || bad "(ii) same-hidden Ambiguous match should FAIL (rc $ov_rc)"
scenario iii EXP-001R Fundamental hidden-dev Fundamental hidden-sealed dev
[ "$ov_rc" = 1 ] && g6line | grep -q '"pass": false, "detail": "1 shared, 0 exempt' \
    && ok "(iii) non-Ambiguous crumb_digest match, different sealed_digest: audit FAIL at G6" \
    || bad "(iii) non-Ambiguous match should FAIL (rc $ov_rc)"
scenario iii-mixed EXP-001R Ambiguous hidden-dev Fundamental hidden-sealed dev
[ "$ov_rc" = 1 ] && g6line | grep -q '"pass": false' \
    && ok "(iii-b) sealed population not Ambiguous (dev side Ambiguous): audit FAIL at G6" \
    || bad "(iii-b) sealed non-Ambiguous should FAIL (rc $ov_rc)"
scenario strict EXP-001 Ambiguous hidden-dev Ambiguous hidden-sealed dev
[ "$ov_rc" = 1 ] && g6line | grep -q '"pass": false, "detail": "1 shared"' \
    && ok "EXP-001 keeps the original strict G6 (same planted match FAILs)" \
    || bad "EXP-001 strict G6 should FAIL the Ambiguous match (rc $ov_rc)"
nb=0
while read -r s; do
    [ -n "$s" ] || continue
    nb=$((nb + 1))
    EXP_ID=EXP-001R "$G" --is-burned "$s" >/dev/null 2>&1 || bad "(iv) burned EXP-001 seed $nb not refused by generate_sealed_data.sh (EXP-001R)"
done <"$here/burned_seeds_exp001.txt"
[ "$nb" = 6 ] && ok "(iv) all 6 burned EXP-001 sealed seeds are refused by generate_sealed_data.sh (EXP-001R)" || bad "(iv) expected 6 burned seeds, found $nb"
if EXP_ID=EXP-001R "$G" --is-burned 12345678901234 >/dev/null 2>&1; then bad "(iv) an unrelated seed was refused"; else ok "(iv) an unrelated seed is not refused"; fi
echo "skipped: dev-vs-dev audit mode (verify_holdout_separation.sh always compares one sealed dir against dev data; no such mode exists)"
echo "blinding self-test: $pass passed, $fail failed"
[ "$fail" = 0 ]
