#!/usr/bin/env bash
# Fail-closed tests for turing-cal-eval (calibration/docs/EVALUATOR.md section 6), on the small committed
# fixture only (seconds, little memory). Every refusal must exit 2 with the named code; the positive runs
# must finish; the dry run must never write under calibration/experiments/EXP-001.
#   test_tc_eval.sh BIN WORKDIR      (writes WORKDIR/det.txt for the plain vs ASan comparison)
set -euo pipefail
BIN="$(realpath "$1")" W="$2"
REPO="$(pwd)"
FIX="$REPO/tests/turing/fixtures/ctr1_seed1_crumbs05-07.ctr"
rm -rf "$W"
mkdir -p "$W"
W="$(realpath "$W")"
# Snapshot of the worktree EXP-001 directory (content hashes), compared at the end.
exp_snap() { (cd "$REPO" && find calibration/experiments/EXP-001 -type f -print0 | sort -z | xargs -0 sha256sum | sha256sum); }
EXP_BEFORE="$(exp_snap)"
pass=0 fail=0
ok() { pass=$((pass + 1)); echo "ok   $1"; }
bad() { fail=$((fail + 1)); echo "FAIL $1"; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

# expect_refuse NAME CODE -- command...
expect_refuse() {
    local name="$1" code="$2"
    shift 3
    local rc=0
    "$@" >"$W/last.out" 2>"$W/last.err" || rc=$?
    if [ "$rc" = 2 ] && grep -q "REFUSED .*: $code:" "$W/last.err"; then ok "$name ($code)"; else
        bad "$name: rc=$rc, wanted $code; stderr: $(head -c 300 "$W/last.err")"
    fi
}

# --- a fake repo: the files the evaluator reads, so tampering never touches the worktree ---
mkrepo() { # mkrepo DIR
    local d="$1"
    mkdir -p "$d/src/turing"
    cp -r "$REPO/calibration" "$d/"
    cp "$REPO"/src/turing/ty_model.[ch] "$REPO"/src/turing/ty_ctr1.[ch] "$REPO"/src/turing/ty_math.[ch] "$d/src/turing/"
    mkdir -p "$d/tools"
    cp "$REPO/tools/turing_cal_eval.c" "$d/tools/"
}
# refresh DIR: after editing the profile, rewrite the sidecar and every hash the manifest pins.
refresh() {
    local d="$1" m="$1/calibration/experiments/EXP-001/candidate_manifest.json" p="$1/calibration/profiles/Turing-profile-v1.0.toml"
    local h
    h="$(sha "$p")"
    printf '%s  Turing-profile-v1.0.toml\n' "$h" >"$d/calibration/profiles/Turing-profile-v1.0.sha256"
    sed -i -E "s/(\"profile_sha256\": \")[0-9a-f]{64}/\1$h/; s/(\"profile_sidecar_sha256\": \")[0-9a-f]{64}/\1$h/" "$m"
    local rel
    while read -r rel; do
        sed -i -E "s#(\"$rel\": \")[0-9a-f]{64}#\1$(sha "$d/$rel")#" "$m"
    done < <(sed -n 's/^ *"\([^"]*\/[^"]*\)": "[0-9a-f]\{64\}",\{0,1\}$/\1/p' "$m")
}
data() { # data DIR: dev layout with seed-1 (group 1) and seed-2 (group 2), both the fixture
    mkdir -p "$1/seed-1/control" "$1/seed-2/control"
    cp "$FIX" "$1/seed-1/control/trace.ctr"
    cp "$FIX" "$1/seed-2/control/trace.ctr"
}
SMALL=B0_uniform,B1_order0,B2_order1,B3_heuristic,M_candidate

# ================= dry-run refusals =================
R="$W/repo"
mkrepo "$R"
data "$W/data"
PH="$(sha "$R/calibration/profiles/Turing-profile-v1.0.toml")"
bash "$R/calibration/scripts/make_dataset_manifest.sh" --dev "$W/data" "1" "2" "$PH" "$W/ds.json" >/dev/null
MAN="$R/calibration/experiments/EXP-001/candidate_manifest.json"
CD="$R/calibration/experiments/EXP-001/candidates"
run_dry() { "$BIN" run --dry-run --repo "$R" --manifest "$MAN" --cand-dir "$CD" --dataset "$W/ds.json" --only "$SMALL" "$@"; }

expect_refuse "dry run out dir under EXP-001" DRY_RUN_TARGET -- run_dry --out "$R/calibration/experiments/EXP-001/x" --work "$W/w0"
[ ! -e "$R/calibration/experiments/EXP-001/x/void_receipt_1.json" ] && ok "no void receipt written under EXP-001" || bad "void receipt under EXP-001"
expect_refuse "dry run work dir under EXP-001" DRY_RUN_TARGET -- run_dry --out "$W/o0" --work "$R/calibration/experiments/EXP-001/w"
expect_refuse "--only outside dry run" ARG -- "$BIN" run --repo "$R" --manifest "$MAN" --cand-dir "$CD" --dataset "$W/ds.json" --only B2_order1 --out "$W/o1" --work "$W/w1"

cp "$CD/B2_order1.tym" "$W/b2.bak"
printf '\x01' | dd of="$CD/B2_order1.tym" bs=1 seek=40 conv=notrunc status=none
expect_refuse "tampered candidate file" DIGEST -- run_dry --out "$W/o2" --work "$W/w2"
[ -f "$W/o2/void_receipt_1.json" ] && ok "void receipt written on refusal" || bad "no void receipt"
cp "$W/b2.bak" "$CD/B2_order1.tym"

cp "$MAN" "$W/man.bak"
sed -i 's/"model_digest": "59ae93/"model_digest": "59ae94/' "$MAN"
expect_refuse "wrong model digest in manifest" MODEL_DIGEST -- run_dry --out "$W/o3" --work "$W/w3"
cp "$W/man.bak" "$MAN"

P="$R/calibration/profiles/Turing-profile-v1.0.toml"
cp "$P" "$W/prof.bak"
sed -i '1s/^/# /' "$P"
expect_refuse "profile byte edited (sidecar mismatch)" PROFILE_DIGEST -- run_dry --out "$W/o4" --work "$W/w4"
refresh "$R"
expect_refuse "profile edited, sidecar refreshed, dataset manifest stale" PROFILE_DIGEST -- run_dry --out "$W/o4b" --work "$W/w4b"
cp "$P" "$W/prof.bak2"
cp "$W/prof.bak" "$P"
refresh "$R"

D="$R/calibration/docs/FAILURE_REPORTING.md"
cp "$D" "$W/doc.bak"
echo " " >>"$D"
expect_refuse "shared background doc changed" DIGEST -- run_dry --out "$W/o5" --work "$W/w5"
cp "$W/doc.bak" "$D"

cp "$W/ds.json" "$W/ds.bak"
sed -i -E '0,/"sha256": "[0-9a-f]/s/("sha256": ")[0-9a-f]/\1f/' "$W/ds.json"
expect_refuse "dataset manifest file sha edited" DATASET_DIGEST -- run_dry --out "$W/o6" --work "$W/w6"
cp "$W/ds.bak" "$W/ds.json"
printf '\x00' | dd of="$W/data/seed-2/control/trace.ctr" bs=1 seek=100 conv=notrunc status=none
expect_refuse "trace bytes changed after the manifest" DATASET_DIGEST -- run_dry --out "$W/o7" --work "$W/w7"
cp "$FIX" "$W/data/seed-2/control/trace.ctr"

# ================= dry-run positive + gate =================
run_dry --out "$W/dry" --work "$W/dryw" 2>"$W/dry.err" && ok "dry run completes" || bad "dry run: $(tail -3 "$W/dry.err")"
expect_refuse "gate mode mismatch" MODE -- "$BIN" gate --bundle "$W/dry" --independent "$W/dry/scorer_primary.json"
"$BIN" gate --dry-run --bundle "$W/dry" --independent "$W/dry/scorer_primary.json" >"$W/gate.out" && ok "dry gate completes" || bad "dry gate"
grep -q '"EXP_001_COMPRESSION_BRIDGE": "DRY_RUN_NOT_EVIDENCE"' "$W/dry/final_receipt.json" && ok "dry receipt is marked not evidence" || bad "dry receipt label"
expect_refuse "gate write-once" EXISTS -- "$BIN" gate --dry-run --bundle "$W/dry" --independent "$W/dry/scorer_primary.json"
expect_refuse "run into a bundle that has a verdict" EXISTS -- run_dry --out "$W/dry" --work "$W/dryw"
grep '"key"' "$W/dry/scorer_primary.json" >"$W/det.txt"

# ================= non-dry (sealed-mode) refusals on a fake frozen setup =================
# Fake frozen repo: profile without FILL_AT_FREEZE, memorizer names bound to small files, manifest frozen.
F="$W/frozen"
mkrepo "$F"
FM="$F/calibration/experiments/EXP-001/candidate_manifest.json"
FP="$F/calibration/profiles/Turing-profile-v1.0.toml"
FC="$F/calibration/experiments/EXP-001/candidates"
sed -i 's/FILL_AT_FREEZE/TEST_ONLY_NOT_A_FREEZE/g' "$FP"
for mem in M_mem M_mem_seed1; do
    cp "$FC/B1_order0.tym" "$FC/$mem.tym"
    line="$(grep '"name": "B1_order0"' "$FM" | sed "s/B1_order0/$mem/g; s/\"location\": \"[^\"]*\"/\"location\": \"test\"/")"
    grep -v "\"name\": \"$mem\"" "$FM" >"$W/fm.tmp"
    awk -v l="$line" '{print} /"name": "M_candidate"/{print l}' "$W/fm.tmp" >"$FM"
done
refresh "$F"
FH="$(sha "$FP")"
COMMIT=1111111111111111111111111111111111111111
bash "$F/calibration/scripts/make_dataset_manifest.sh" --dev "$W/data" "1" "2" "$FH" "$W/fds.json" >/dev/null
sed -i -E "s/\"split\": \"development\"/\"split\": \"sealed_test\"/; s/\"freeze_commit\": \"NONE\"/\"freeze_commit\": \"$COMMIT\"/; s/\"released_utc\": \"NOT_SEALED\"/\"released_utc\": \"2026-10-01T12:00:00Z\"/" "$W/fds.json"
printf '{\n  "schema": "turing.cal.overlap_audit.v1",\n  "freeze_commit": "%s",\n  "result": "PASS"\n}\n' "$COMMIT" >"$W/ov.json"
run_sealed() { "$BIN" run --repo "$F" --manifest "$FM" --cand-dir "$FC" "$@"; }

expect_refuse "draft manifest in sealed mode" NOT_FROZEN -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s1" --work "$W/sw1"
sed -i -E "s/\"status\": \"draft\"/\"status\": \"frozen\"/; s/(\"git_head\": \")[0-9a-f]{40}/\1$COMMIT/" "$FM"
sed -i "s/^  \"status\": \"frozen\",/&\n  \"frozen_at\": \"2026-10-01T11:00:00Z\",/" "$FM"
sed -i -E "s/(\"turing-cal-eval\": \")[0-9a-f]{64}/\1$(printf '0%.0s' $(seq 64))/" "$FM"
expect_refuse "evaluator binary not the frozen runtime" RUNTIME_DIGEST -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s2" --work "$W/sw2"
sed -i -E "s/(\"turing-cal-eval\": \")[0-9a-f]{64}/\1$(sha "$BIN")/" "$FM"
cp "$W/fds.json" "$W/fds.ok"
sed -i 's/"split": "sealed_test"/"split": "development"/' "$W/fds.json"
expect_refuse "development split in sealed mode" SPLIT -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s3" --work "$W/sw3"
cp "$W/fds.ok" "$W/fds.json"
expect_refuse "no overlap audit" OVERLAP -- run_sealed --dataset "$W/fds.json" --out "$W/s4" --work "$W/sw4"
sed 's/"PASS"/"FAIL"/' "$W/ov.json" >"$W/ovf.json"
expect_refuse "overlap audit FAIL" OVERLAP -- run_sealed --dataset "$W/fds.json" --overlap "$W/ovf.json" --out "$W/s5" --work "$W/sw5"
sed "s/$COMMIT/2222222222222222222222222222222222222222/" "$W/fds.json" >"$W/fds2.json"
sed "s/$COMMIT/2222222222222222222222222222222222222222/" "$W/ov.json" >"$W/ov2.json"
expect_refuse "manifest git_head is not the sealed freeze commit" FREEZE_COMMIT -- run_sealed --dataset "$W/fds2.json" --overlap "$W/ov2.json" --out "$W/s6" --work "$W/sw6"
sed 's/"released_utc": "2026-10-01T12:00:00Z"/"released_utc": "2026-10-01T10:00:00Z"/' "$W/fds.json" >"$W/fds3.json"
expect_refuse "candidate frozen after the data release" FREEZE_AFTER_RELEASE -- run_sealed --dataset "$W/fds3.json" --overlap "$W/ov.json" --out "$W/s7" --work "$W/sw7"
cp "$FP" "$W/fp.bak"
sed -i 's/TEST_ONLY_NOT_A_FREEZE/FILL_AT_FREEZE/' "$FP"
refresh "$F"
expect_refuse "profile still has FILL_AT_FREEZE" NOT_FROZEN -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s8" --work "$W/sw8"
cp "$W/fp.bak" "$FP"
refresh "$F"

# Positive sealed-mode run on the fake setup, then S8 checks at the gate.
run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/sealed" --work "$W/sealedw" 2>"$W/sealed.err" &&
    ok "sealed-mode run completes on the fake frozen setup" || bad "sealed-mode run: $(tail -3 "$W/sealed.err")"
expect_refuse "self-comparison as independent scorer (sealed mode)" NOT_INDEPENDENT -- "$BIN" gate --bundle "$W/sealed" --independent "$W/sealed/scorer_primary.json"
sed '0,/"value": /s/"value": \([0-9]\)/"value": 9\1/' "$W/sealed/scorer_primary.json" | sed 's/(primary)/(test copy, one value changed)/' >"$W/indep_bad.json"
"$BIN" gate --bundle "$W/sealed" --independent "$W/indep_bad.json" >"$W/gate2.out" || true
grep -q '"S8": "FAIL"' "$W/sealed/final_receipt.json" && grep -q '"verdict": "FAIL"' "$W/sealed/final_receipt.json" &&
    ok "independent scorer mismatch gives S8 FAIL and verdict FAIL" || bad "S8 mismatch: $(cat "$W/gate2.out")"
grep -q '"EXP_001_COMPRESSION_BRIDGE": "FAIL"' "$W/sealed/final_receipt.json" && ok "bridge line written" || bad "bridge line"

# Verdict rule at the gate (prereg section 5a): criteria forced in a pending receipt, S8 from a matching
# independent file with a different header (so not byte-identical).
verdict_case() { # verdict_case NAME S6 S7 S9 EXPECTED
    local b="$W/v_$1"
    run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$b" --work "$W/vw" 2>/dev/null || { bad "verdict case $1 run"; return; }
    sed -i -E "s/\"S6\": \"[A-Z_]+\"/\"S6\": \"$2\"/; s/\"S7\": \"[A-Z_]+\"/\"S7\": \"$3\"/; s/\"S9\": \"[A-Z_]+\"/\"S9\": \"$4\"/" "$b/final_receipt.pending.json"
    sed 's/(primary)/(test copy, same values)/' "$b/scorer_primary.json" >"$W/indep_ok.json"
    "$BIN" gate --bundle "$b" --independent "$W/indep_ok.json" >/dev/null 2>&1 || true
    grep -q "\"verdict\": \"$5\"" "$b/final_receipt.json" && grep -q '"S8": "PASS"' "$b/final_receipt.json" &&
        ok "verdict rule: S6 $2, S7 $3, S9 $4 -> $5" || bad "verdict rule case $1"
}
verdict_case all_pass PASS PASS PASS PASS
verdict_case s6_straddle INCONCLUSIVE PASS PASS INCONCLUSIVE
verdict_case s9_straddle PASS PASS INCONCLUSIVE INCONCLUSIVE
verdict_case wrong_side_beats_straddle INCONCLUSIVE FAIL PASS FAIL

# The worktree's EXP-001 directory is untouched by all of the above.
[ "$(exp_snap)" = "$EXP_BEFORE" ] && ok "calibration/experiments/EXP-001 unchanged" ||
    bad "calibration/experiments/EXP-001 changed"

echo "turing-cal-eval fail-closed tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
