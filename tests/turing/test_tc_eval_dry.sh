#!/usr/bin/env bash
# EXP-001 evaluator dry run on DEVELOPMENT data (not evidence): group 1 = dev seed 7, group 2 = dev seed 6,
# all seven candidates. The run root OUT has the sealed-run layout of calibration/docs/DATA_FORMAT.md section 4
# (candidate_manifest.json, dataset_manifest.json, bundle/, work/, run with cwd = OUT). Order: run, then the
# independent scorer (tools/turing_verify_indep, reads only the published docs, the bundle and the CTR1 files),
# then the gate with the independent scorer's output. Requires verdict PASS with S8 PASS, M_candidate T > 0,
# both memorizers T < 0. Writes only under OUT (never calibration/experiments/EXP-001).
#   test_tc_eval_dry.sh BIN OUTDIR DEV_RUN_DIR BIG_CANDIDATE_DIR [INDEP_BIN]
set -euo pipefail
REPO="$(pwd)"
BIN="$(realpath "$1")" OUT="$2" DATA="$(realpath "$3")" BIG="$(realpath "$4")"
IND="$(realpath "${5:-build/turing-verify-indep/indep-scorer}")"
die() { echo "eval dry run: FAIL: $*" >&2; exit 1; }
[ ! -e "$HOME/workspace/.spark-quiet" ] || die ".spark-quiet is set"
before="$(git status --porcelain -- calibration/experiments/EXP-001)"
rm -rf "$OUT"
mkdir -p "$OUT/docs/profiles"
OUT="$(realpath "$OUT")"
MAN="$REPO/calibration/experiments/EXP-001/candidate_manifest.json"
SMALL="$REPO/calibration/experiments/EXP-001/candidates"
cp "$MAN" "$OUT/candidate_manifest.json"
# The docs directory: what a docs-only reader has (the published profile and candidate manifest).
cp "$REPO/calibration/profiles/Turing-profile-v1.0.toml" "$OUT/docs/profiles/"
cp "$MAN" "$OUT/docs/candidate_manifest.json"
P="$(cut -c1-64 calibration/profiles/Turing-profile-v1.0.sha256)"
bash calibration/scripts/make_dataset_manifest.sh --dev "$DATA" "7" "6" "$P" "$OUT/dataset_manifest.json"
cd "$OUT"
/usr/bin/time -v "$BIN" run --dry-run --repo "$REPO" --manifest "$MAN" --cand-dir "$SMALL" --cand-dir "$BIG" \
    --dataset dataset_manifest.json --out bundle --work work 2>run.stderr || { cat run.stderr; die "run refused"; }
grep -E 'Elapsed|Maximum resident' run.stderr
/usr/bin/time -v "$IND" --docs docs --bundle-root . --cand-dir "$SMALL" --cand-dir "$BIG" \
    --out scorer_independent.json --details indep_details.json 2>indep.stderr || { tail -20 indep.stderr; die "independent scorer failed"; }
grep -E 'Elapsed|Maximum resident' indep.stderr
cp scorer_independent.json bundle/scorer_independent.json
"$BIN" gate --dry-run --bundle bundle --independent bundle/scorer_independent.json
grep -q '"S8": "PASS"' bundle/final_receipt.json || die "S8 not PASS: the independent scorer disagrees (see indep_details.json)"
grep -q '"verdict": "PASS"' bundle/final_receipt.json || die "verdict rule not PASS: $(grep -o '"criteria": {[^}]*}' bundle/final_receipt.json)"
val() { sed -n "s/.*\"key\": \"$1\", \"value\": \(-\{0,1\}[0-9]*\)}.*/\1/p" bundle/scorer_primary.json; }
for g in 1 2; do
    for m in ideal range rans; do
        v="$(val "g$g.M_candidate.T_${m}_vs_B2.lo_ub")"
        [ -n "$v" ] && [ "$v" -gt 0 ] || die "g$g M_candidate T_$m lower bound $v not > 0"
        for mem in M_mem M_mem_seed1; do
            v="$(val "g$g.$mem.T_${m}_vs_B2.hi_ub")"
            [ -n "$v" ] && [ "$v" -lt 0 ] || die "g$g $mem T_$m upper bound $v not < 0"
        done
    done
done
cd "$REPO"
[ "$(git status --porcelain -- calibration/experiments/EXP-001)" = "$before" ] || die "the dry run changed calibration/experiments/EXP-001"
du -sh "$OUT/work" | sed 's/^/work dir size: /'
rm -rf "$OUT/work"
echo "eval dry run: PASS (verdict rule PASS, S8 PASS from the independent scorer, M_candidate T > 0, memorizers T < 0)"
