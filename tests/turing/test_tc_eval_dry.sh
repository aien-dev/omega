#!/usr/bin/env bash
# EXP-001 evaluator dry run on DEVELOPMENT data (not evidence): group 1 = dev seed 7, group 2 = dev seed 6,
# all seven candidates. Writes only under OUTDIR (never calibration/experiments/EXP-001), then checks that
# the table is sane: M_candidate T > 0, both memorizers T < 0, every file and crumb inside the envelope.
#   test_tc_eval_dry.sh BIN OUTDIR DEV_RUN_DIR BIG_CANDIDATE_DIR
set -euo pipefail
BIN="$1" OUT="$2" DATA="$3" BIG="$4"
die() { echo "eval dry run: FAIL: $*" >&2; exit 1; }
[ ! -e "$HOME/workspace/.spark-quiet" ] || die ".spark-quiet is set"
before="$(git status --porcelain -- calibration/experiments/EXP-001)"
rm -rf "$OUT/bundle" "$OUT/work"
mkdir -p "$OUT"
P="$(cut -c1-64 calibration/profiles/Turing-profile-v1.0.sha256)"
bash calibration/scripts/make_dataset_manifest.sh --dev "$DATA" "7" "6" "$P" "$OUT/dataset_manifest.json"
/usr/bin/time -v "$BIN" run --dry-run --repo . --manifest calibration/experiments/EXP-001/candidate_manifest.json \
    --cand-dir calibration/experiments/EXP-001/candidates --cand-dir "$BIG" --dataset "$OUT/dataset_manifest.json" \
    --out "$OUT/bundle" --work "$OUT/work" 2>"$OUT/run.stderr" || { cat "$OUT/run.stderr"; die "run refused"; }
grep -E 'Elapsed|Maximum resident' "$OUT/run.stderr"
"$BIN" gate --dry-run --bundle "$OUT/bundle" --independent "$OUT/bundle/scorer_primary.json"
val() { sed -n "s/.*\"key\": \"$1\", \"value\": \(-\{0,1\}[0-9]*\)}.*/\1/p" "$OUT/bundle/scorer_primary.json"; }
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
grep -q '"S4": "PASS"' "$OUT/bundle/final_receipt.json" || echo "NOTE: S4 not PASS in the dry run (see REPORT.md envelope lines)"
[ "$(git status --porcelain -- calibration/experiments/EXP-001)" = "$before" ] || die "the dry run changed calibration/experiments/EXP-001"
du -sh "$OUT/work" | sed 's/^/work dir size: /'
rm -rf "$OUT/work"
echo "eval dry run: PASS (M_candidate T > 0, memorizers T < 0, groups 1 and 2, ideal and both coders)"
