#!/usr/bin/env bash
# EXP-001 evaluator dry run on DEVELOPMENT data (not evidence): group 1 = dev seed 7, group 2 = dev seed 6,
# all seven candidates. The run root OUT has the sealed-run layout of calibration/docs/DATA_FORMAT.md section 4
# (candidate_manifest.json, dataset_manifest.json, bundle/, work/, run with cwd = OUT). Order: run, then the
# independent scorer (tools/turing_verify_indep, reads only the published docs, the bundle and the CTR1 files),
# then the gate with the independent scorer's output. Requires verdict PASS with S8 PASS, M_candidate T > 0,
# both memorizers T < 0. Writes only under OUT (never calibration/experiments/EXP-001).
#   test_tc_eval_dry.sh BIN OUTDIR DEV_RUN_DIR BIG_CANDIDATE_DIR [INDEP_BIN]
# Experiment: env EXP_ID = EXP-001 (default, Turing-profile-v1.0) or EXP-001R (Turing-profile-v1.1), the same variable
# the calibration scripts, the evaluator and the independent scorer read. EXP-001R has no committed candidate
# manifest before its freeze, so the dry run derives one from the EXP-001 manifest (same seven candidates, this
# experiment id and profile digest) inside OUT only.
set -euo pipefail
REPO="$(pwd)"
BIN="$(realpath "$1")" OUT="$2" DATA="$(realpath "$3")" BIG="$(realpath "$4")"
IND="$(realpath "${5:-build/turing-verify-indep/indep-scorer}")"
die() { echo "eval dry run: FAIL: $*" >&2; exit 1; }
[ ! -e "$HOME/workspace/.spark-quiet" ] || die ".spark-quiet is set"
. calibration/scripts/tc_exp.sh
before="$(git status --porcelain -- calibration/experiments/EXP-001 calibration/experiments/EXP-001R)"
rm -rf "$OUT"
mkdir -p "$OUT/docs/profiles"
OUT="$(realpath "$OUT")"
SRCMAN="$REPO/calibration/experiments/EXP-001/candidate_manifest.json"
P="$(cut -c1-64 "$REPO/$EXP_SIDECAR")"
SRC2="$OUT/candidate_manifest_id.json"
if [ "$EXP_ID" = EXP-001 ]; then cp "$SRCMAN" "$SRC2"; else
    sed -e "s/\"experiment\": \"EXP-001\"/\"experiment\": \"$EXP_ID\"/" -e "s|Turing-profile-v1.0|$EXP_PROFILE_ID|g" \
        -e "s/\"profile_sha256\": \"[0-9a-f]*\"/\"profile_sha256\": \"$P\"/" -e "s/\"profile_sidecar_sha256\": \"[0-9a-f]*\"/\"profile_sidecar_sha256\": \"$P\"/" "$SRCMAN" >"$SRC2"
fi
# The frozen EXP-001 manifest pins the exact bytes of every shared-background file, including the evaluator source
# and the protocol docs, so it cannot be used unchanged once any of them is revised. A dry run is not evidence:
# its manifest is a copy in OUT with the shared-background hashes recomputed from the current tree.
MAN="$OUT/candidate_manifest_src.json"
sect=0
while IFS= read -r line; do
    case "$line" in
    *'"shared_background_sha256": {'*) sect=1 ;;
    *'}'*) sect=0 ;;
    *) if [ $sect = 1 ]; then
           key="${line#*\"}"; key="${key%%\"*}"; com=""; case "$line" in *,) com="," ;; esac
           line="$(printf '    "%s": "%s"%s' "$key" "$(sha256sum "$REPO/$key" | cut -c1-64)" "$com")"
       fi ;;
    esac
    printf '%s\n' "$line"
done <"$SRC2" >"$MAN"
SMALL="$REPO/calibration/experiments/EXP-001/candidates"
cp "$MAN" "$OUT/candidate_manifest.json"
# The docs directory: what a docs-only reader has (the published profile and candidate manifest).
cp "$REPO/$EXP_PROFILE" "$OUT/docs/profiles/"
cp "$MAN" "$OUT/docs/candidate_manifest.json"
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
grep -q '"dry_run": true' scorer_independent.json || die "independent scorer did not report dry_run true on development data"
# The independent scorer takes dry_run from the dataset manifest split: a sealed_test split must give false.
mkdir -p sealedchk
sed 's/"split": "development"/"split": "sealed_test"/' dataset_manifest.json >sealedchk/dataset_manifest.json
cp candidate_manifest.json sealedchk/
ln -sfn "$OUT/bundle" sealedchk/bundle; ln -sfn "$OUT/work" sealedchk/work; ln -sfn "$OUT/docs" sealedchk/docs
(cd sealedchk && "$IND" --docs docs --bundle-root . --cand-dir "$SMALL" --cand-dir "$BIG" \
    --out s.json --details d.json 2>e.txt) || { tail -5 sealedchk/e.txt; die "independent scorer failed on the sealed_test split copy"; }
grep -q '"dry_run": false' sealedchk/s.json || die "independent scorer did not report dry_run false for split sealed_test"
rm -rf sealedchk
cd "$REPO"
[ "$(git status --porcelain -- calibration/experiments/EXP-001 calibration/experiments/EXP-001R)" = "$before" ] || die "the dry run changed calibration/experiments"
du -sh "$OUT/work" | sed 's/^/work dir size: /'
rm -rf "$OUT/work"
echo "eval dry run ($EXP_ID): PASS (verdict rule PASS, S8 PASS from the independent scorer, M_candidate T > 0, memorizers T < 0)"
