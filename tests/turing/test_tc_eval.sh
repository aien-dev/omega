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

# expect_final NAME CODE -- command...: a freeze-order violation in sealed mode is S2 FAIL, final (exit 1,
# final_receipt.json kind terminal_fail with S2 FAIL), never a void. The out dir is the last --out argument.
expect_final() {
    local name="$1" code="$2"
    shift 3
    local rc=0 o="" prev=""
    for a in "$@"; do [ "$prev" = --out ] && o="$a"; prev="$a"; done
    "$@" >"$W/last.out" 2>"$W/last.err" || rc=$?
    if [ "$rc" = 1 ] && grep -q "FAILED .*: $code:" "$W/last.err" && grep -q '"S2": "FAIL"' "$o/final_receipt.json" &&
        grep -q '"kind": "terminal_fail"' "$o/final_receipt.json" && [ ! -e "$o/void_receipt_1.json" ]; then ok "$name ($code, S2 FAIL final)"; else
        bad "$name: rc=$rc, wanted terminal $code; stderr: $(head -c 300 "$W/last.err")"
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
    cp -r "$REPO/tools/turing_verify_indep" "$d/tools/" && rm -rf "$d/tools/turing_verify_indep/build"
    # The fixture manifest pins the shared-background files (docs, scripts) by hash; those files change
    # legitimately after C_f, so re-pin the fixture copy only (the committed EXP-001 manifest is untouched).
    refresh "$d"
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
# Experiment selection (env EXP_ID, same as the scripts): an unknown id is refused; EXP-001R forbids its own dir in a dry run.
rc=0; EXP_ID=EXP-002 "$BIN" run >"$W/last.out" 2>"$W/last.err" || rc=$?
{ [ "$rc" = 2 ] && grep -q "unknown EXP_ID" "$W/last.err"; } && ok "unknown EXP_ID refused" || bad "unknown EXP_ID: rc=$rc"
EXP_ID=EXP-001R expect_refuse "EXP-001R dry run out dir under EXP-001R" DRY_RUN_TARGET -- run_dry --out "$R/calibration/experiments/EXP-001R/x" --work "$W/w0"
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
# Fake frozen repo: profile without FILL_AT_FREEZE, memorizer names bound to small files. The two-step freeze
# (BLINDING_PROTOCOL.md section 2) is played out for real: the commit that holds the frozen manifest is C_f,
# refs/remotes/origin/main points at it, and the sealed seeds are derived from C_f and the profile digest.
F="$W/frozen"
mkrepo "$F"
FM="$F/calibration/experiments/EXP-001/candidate_manifest.json"
FP="$F/calibration/profiles/Turing-profile-v1.0.toml"
FC="$F/calibration/experiments/EXP-001/candidates"
# The committed EXP-001 files are already frozen. The fixture replays the freeze from the draft state, so put the
# fixture copy (never the committed files) back to draft: manifest and preregistration draft, runtime_digest unfilled,
# and no published receipts.
rm -f "$F/calibration/experiments/EXP-001/freeze_receipt.json" "$F/calibration/experiments/EXP-001/final_receipt.json"
sed -i -E '/^  "frozen_at": /d; s/"status": "frozen"/"status": "draft"/' "$FM" "$F/calibration/experiments/EXP-001/preregistration.json"
sed -i -E 's/^(runtime_digest = )"[0-9a-f]{64}"/\1"FILL_AT_FREEZE"/' "$FP"
sed -i 's/FILL_AT_FREEZE/TEST_ONLY_NOT_A_FREEZE/g' "$FP"
for mem in M_mem M_mem_seed1; do
    cp "$FC/B1_order0.tym" "$FC/$mem.tym"
    line="$(grep '"name": "B1_order0"' "$FM" | sed "s/B1_order0/$mem/g; s/\"location\": \"[^\"]*\"/\"location\": \"test\"/")"
    grep -v "\"name\": \"$mem\"" "$FM" >"$W/fm.tmp"
    awk -v l="$line" '{print} /"name": "M_candidate"/{print l}' "$W/fm.tmp" >"$FM"
done
sed -i "s/\"status\": \"draft\"/\"status\": \"frozen\"/" "$F/calibration/experiments/EXP-001/preregistration.json"
refresh "$F"
FH="$(sha "$FP")"
# Sealed mode needs --repo to be a clean git checkout: commit the fake repo before every sealed run.
commit_f() { (cd "$F" && { [ -d .git ] || git init -q; } && git add -A && git -c user.name=tce-test -c user.email=tce@test -c commit.gpgsign=false commit -q --allow-empty -m fixture); }
run_sealed_raw() { "$BIN" run --repo "$F" --manifest "$FM" --cand-dir "$FC" "$@"; }
run_sealed() { commit_f; run_sealed_raw "$@"; }
# addp: an independent scorer file must report "problems": 0 (EVALUATOR.md section 6); test copies of the primary add it.
addp() { sed "/\"dataset_manifest_sha256\"/a\\  \"problems\": 0,"; }
# seed_of COMMIT PROFILE G J: the turing.cal.sealed.v1 seed rule, as written in generate_sealed_data.sh.
seed_of() {
    local h
    h="$(printf '%s' "turing.cal.sealed.v1|$1|$2|g$3|$4" | sha256sum | cut -c1-16)"
    printf '%d' "$((16#$(printf '%x' $((16#${h:0:1} & 7)))${h:1}))"
}
# sealed_ds OUT COMMIT S1 S2 RELEASED: a sealed-split dataset manifest over the fixture, seeds S1 (group 1), S2 (group 2).
sealed_ds() {
    rm -rf "$W/sd" && mkdir -p "$W/sd/seed-$3/control" "$W/sd/seed-$4/control"
    cp "$FIX" "$W/sd/seed-$3/control/trace.ctr" && cp "$FIX" "$W/sd/seed-$4/control/trace.ctr"
    bash "$F/calibration/scripts/make_dataset_manifest.sh" --dev "$W/sd" "$3" "$4" "$FH" "$1" >/dev/null
    sed -i -E "s/\"split\": \"development\"/\"split\": \"sealed_test\"/; s/\"freeze_commit\": \"NONE\"/\"freeze_commit\": \"$2\"/; s/\"released_utc\": \"NOT_SEALED\"/\"released_utc\": \"$5\"/" "$1"
    # keep the trace files: the evaluator re-hashes them by path
    rm -rf "$W/sd_$(basename "$1")" && mv "$W/sd" "$W/sd_$(basename "$1")"
    sed -i "s#$W/sd/#$W/sd_$(basename "$1")/#g" "$1"
}
bash "$F/calibration/scripts/make_dataset_manifest.sh" --dev "$W/data" "1" "2" "$FH" "$W/pre.json" >/dev/null
# The pre-freeze refusals below need a 40-hex freeze_commit (receipt binding, Q5) and an out path ending in
# /<C_f>/run/bundle (one VOID counter, Q1); CF0 stands in for C_f.
CF0=1111111111111111111111111111111111111111
sed "s/\"freeze_commit\": \"NONE\"/\"freeze_commit\": \"$CF0\"/" "$W/pre.json" >"$W/pre1.json"
expect_refuse "sealed run, dataset without a 40-hex freeze_commit (no receipt)" FREEZE_COMMIT -- run_sealed_raw --dataset "$W/pre.json" --out "$W/sp/$CF0/run/bundle" --work "$W/swp"
[ -z "$(ls -A "$W/sp" 2>/dev/null)" ] && ok "pre-pass refusal writes nothing" || bad "pre-pass refusal wrote files"

expect_refuse "sealed run, --repo not a git checkout" DIRTY_TREE -- run_sealed_raw --dataset "$W/pre1.json" --out "$W/s0/$CF0/run/bundle" --work "$W/sw0"
commit_f
touch "$F/uncommitted.txt"
expect_refuse "sealed run, uncommitted change in --repo" DIRTY_TREE -- run_sealed_raw --dataset "$W/pre1.json" --out "$W/s0b/$CF0/run/bundle" --work "$W/sw0b"
rm -f "$F/uncommitted.txt"
expect_refuse "draft manifest in sealed mode" NOT_FROZEN -- run_sealed --dataset "$W/pre1.json" --out "$W/s1/$CF0/run/bundle" --work "$W/sw1"

# Step 1 of the freeze: the frozen manifest (no commit named inside it) is committed; that commit is C_f.
sed -i -E "s/\"status\": \"draft\"/\"status\": \"frozen\"/" "$FM"
sed -i "s/^  \"status\": \"frozen\",/&\n  \"frozen_at\": \"2026-09-01T00:00:00Z\",/" "$FM"
sed -i -E "s/(\"independent_scorer_source_sha256\": \")[0-9a-f]{64}/\1$(sh "$F/calibration/scripts/indep_source_digest.sh")/" "$FM"
sed -i -E "s/(\"turing-cal-eval\": \")[0-9a-f]{64}/\1$(printf '0%.0s' $(seq 64))/" "$FM"
cp "$FM" "$W/fm_badrt"
sed -i -E "s/(\"turing-cal-eval\": \")[0-9a-f]{64}/\1$(sha "$BIN")/" "$FM"
commit_f
CF="$(git -C "$F" rev-parse HEAD)"
git -C "$F" update-ref refs/remotes/origin/main "$CF"
REL=2030-01-01T00:00:00Z
sealed_ds "$W/fds.json" "$CF" "$(seed_of "$CF" "$FH" 1 0)" "$(seed_of "$CF" "$FH" 2 0)" "$REL"
printf '{\n  "schema": "turing.cal.overlap_audit.v1",\n  "freeze_commit": "%s",\n  "result": "PASS"\n}\n' "$CF" >"$W/ov.json"

cp "$FM" "$W/fm.good"
cp "$W/fm_badrt" "$FM"
expect_refuse "evaluator binary not the frozen runtime" RUNTIME_DIGEST -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s2/$CF/run/bundle" --work "$W/sw2"
cp "$W/fm.good" "$FM"
cp "$W/fds.json" "$W/fds.ok"
sed -i 's/"split": "sealed_test"/"split": "development"/' "$W/fds.json"
expect_refuse "development split in sealed mode" SPLIT -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s3/$CF/run/bundle" --work "$W/sw3"
cp "$W/fds.ok" "$W/fds.json"
expect_refuse "no overlap audit" OVERLAP -- run_sealed --dataset "$W/fds.json" --out "$W/s4/$CF/run/bundle" --work "$W/sw4"
sed 's/"PASS"/"FAIL"/' "$W/ov.json" >"$W/ovf.json"
expect_final "overlap audit FAIL" OVERLAP -- run_sealed --dataset "$W/fds.json" --overlap "$W/ovf.json" --out "$W/s5/$CF/run/bundle" --work "$W/sw5"
sed "s/$CF/2222222222222222222222222222222222222222/" "$W/ov.json" >"$W/ov2.json"
expect_refuse "overlap audit names another commit than C_f" FREEZE_COMMIT -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov2.json" --out "$W/s6/$CF/run/bundle" --work "$W/sw6"
git -C "$F" update-ref refs/remotes/origin/main "$CF^"
expect_refuse "C_f is not an ancestor of origin/main" FREEZE_COMMIT -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s6b/$CF/run/bundle" --work "$W/sw6b"
git -C "$F" update-ref -d refs/remotes/origin/main
expect_refuse "no origin/main in --repo" FREEZE_COMMIT -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s6c/$CF/run/bundle" --work "$W/sw6c"
git -C "$F" update-ref refs/remotes/origin/main "$CF"
sed -i 's/"fit_data": "exp-/"fit_data": "EDITED exp-/' "$FM"
expect_final "candidate manifest edited after C_f" FREEZE_COMMIT -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s6d/$CF/run/bundle" --work "$W/sw6d"
cp "$W/fm.good" "$FM"
sealed_ds "$W/fds_seed.json" "$CF" "$(seed_of "$CF" "$FH" 1 0)" 12345 "$REL"
expect_final "group 2 seed not derived from C_f" SEED_DERIVATION -- run_sealed --dataset "$W/fds_seed.json" --overlap "$W/ov.json" --out "$W/s6e/$CF/run/bundle" --work "$W/sw6e"
sed 's/"released_utc": "[^"]*"/"released_utc": "2026-09-02T00:00:00Z"/' "$W/fds.json" >"$W/fds_ct.json"
expect_final "data released before the C_f commit time" FREEZE_AFTER_RELEASE -- run_sealed --dataset "$W/fds_ct.json" --overlap "$W/ov.json" --out "$W/s7b/$CF/run/bundle" --work "$W/sw7b"
sed 's/"released_utc": "[^"]*"/"released_utc": "2026-08-01T00:00:00Z"/' "$W/fds.json" >"$W/fds3.json"
expect_final "candidate frozen_at after the data release" FREEZE_AFTER_RELEASE -- run_sealed --dataset "$W/fds3.json" --overlap "$W/ov.json" --out "$W/s7/$CF/run/bundle" --work "$W/sw7"
echo "revised after the freeze" >>"$F/tools/turing_verify_indep/SPEC_GAPS.md"
expect_refuse "independent scorer source revised after C_f" INDEP_SOURCE -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s7c/$CF/run/bundle" --work "$W/sw7c"
cp "$REPO/tools/turing_verify_indep/SPEC_GAPS.md" "$F/tools/turing_verify_indep/SPEC_GAPS.md"
cp "$FP" "$W/fp.bak"
sed -i 's/TEST_ONLY_NOT_A_FREEZE/FILL_AT_FREEZE/' "$FP"
refresh "$F"
expect_refuse "profile still has FILL_AT_FREEZE" NOT_FROZEN -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/s8/$CF/run/bundle" --work "$W/sw8"
cp "$W/fp.bak" "$FP"
refresh "$F"
cp "$W/fm.good" "$FM"

# Void attempts (FAILURE_REPORTING.md section 2): each void is kept and numbered; a retry must be byte-identical;
# the third void attempt in sealed mode ends the experiment as INCONCLUSIVE (INFRA).
expect_refuse "void attempt 1" OVERLAP -- run_sealed --dataset "$W/fds.json" --out "$W/rv/$CF/run/bundle" --work "$W/rvw"
expect_refuse "retry with a different command" RETRY_DIFFERS -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/rv/$CF/run/bundle" --work "$W/rvw"
[ ! -e "$W/rv/$CF/run/bundle/final_receipt.json" ] && ok "two void attempts: no verdict yet" || bad "verdict after two void attempts"
expect_refuse "void attempt 3 (identical retry)" OVERLAP -- run_sealed --dataset "$W/fds.json" --out "$W/rv/$CF/run/bundle" --work "$W/rvw"
grep -q '"attempt": 3' "$W/rv/$CF/run/bundle/void_receipt_3.json" && grep -q '"command": "' "$W/rv/$CF/run/bundle/void_receipt_1.json" &&
    grep -q '"verdict": "INCONCLUSIVE"' "$W/rv/$CF/run/bundle/final_receipt.json" && grep -q '"reason": "INFRA"' "$W/rv/$CF/run/bundle/final_receipt.json" &&
    ok "third void attempt writes INCONCLUSIVE (INFRA), all void receipts kept" || bad "INCONCLUSIVE INFRA after 3 voids"
expect_refuse "no fourth attempt" ATTEMPTS -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/rv/$CF/run/bundle" --work "$W/rvw"

# Positive sealed-mode run on the fake setup, then S8 checks at the gate.
run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/sealed/$CF/run/bundle" --work "$W/sealedw" 2>"$W/sealed.err" &&
    ok "sealed-mode run completes on the fake frozen setup" || bad "sealed-mode run: $(tail -3 "$W/sealed.err")"
grep -q '"notebook": {"operator": "[^"]*", "host": "[^"]*", "kernel": "[^"]*", "commit": "[0-9a-f]\{40\}", "tree_clean": true}' "$W/sealed/$CF/run/bundle/final_receipt.pending.json" &&
    ok "sealed receipt records operator, host, kernel, commit, clean tree" || bad "notebook record missing in sealed receipt"
expect_refuse "self-comparison as independent scorer (sealed mode)" NOT_INDEPENDENT -- "$BIN" gate --bundle "$W/sealed/$CF/run/bundle" --independent "$W/sealed/$CF/run/bundle/scorer_primary.json"
sed '0,/"value": /s/"value": \([0-9]\)/"value": 9\1/' "$W/sealed/$CF/run/bundle/scorer_primary.json" | sed 's/(primary)/(test copy, one value changed)/' | addp >"$W/indep_bad.json"
"$BIN" gate --bundle "$W/sealed/$CF/run/bundle" --independent "$W/indep_bad.json" >"$W/gate2.out" || true
grep -q '"S8": "FAIL"' "$W/sealed/$CF/run/bundle/final_receipt.json" && grep -q '"verdict": "FAIL"' "$W/sealed/$CF/run/bundle/final_receipt.json" &&
    ok "independent scorer mismatch gives S8 FAIL and verdict FAIL" || bad "S8 mismatch: $(cat "$W/gate2.out")"
grep -q '"EXP_001_COMPRESSION_BRIDGE": "FAIL"' "$W/sealed/$CF/run/bundle/final_receipt.json" && ok "bridge line written" || bad "bridge line"

# Receipt binding (CAL-0 review 2 Q5): void and terminal receipts name the profile digest, C_f and the candidate
# manifest digest.
CM_GOOD="$(sha "$W/fm.good")"
bind_ok() { grep -q "\"profile_digest\": \"$FH\"," "$1" && grep -q "\"freeze_commit\": \"$CF\"," "$1" && grep -q "\"candidate_manifest_sha256\": \"$CM_GOOD\"" "$1"; }
bind_ok "$W/rv/$CF/run/bundle/void_receipt_1.json" && bind_ok "$W/rv/$CF/run/bundle/final_receipt.json" &&
    grep -q '"stage": "evaluation"' "$W/rv/$CF/run/bundle/void_receipt_1.json" && bind_ok "$W/s5/$CF/run/bundle/final_receipt.json" &&
    ok "void, INCONCLUSIVE and terminal receipts carry profile_digest, freeze_commit, candidate_manifest_sha256" || bad "receipt binding fields"
# One VOID counter (Q6/Q1): a sealed bundle outside <eval root>/<C_f>/run/bundle is refused.
expect_refuse "sealed --out not <root>/<C_f>/run/bundle" OUT_PATH -- run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$W/elsewhere" --work "$W/sweo"
# Q1: voids of the frozen-tree tests (record_void.sh) and of the evaluator share one counter; the third ends EXP-001.
RVS="$F/calibration/scripts/record_void.sh"
ER="$W/er1/$CF/run/bundle"
rc=0; TC_EVAL_ROOT="$W/er1" bash "$RVS" --repo "$F" --commit "$CF" --step 7 --code BUILD --reason "test build failed" --command "make x" 2>/dev/null || rc=$?
[ "$rc" = 0 ] && grep -q '"stage": "tests"' "$ER/void_receipt_1.json" && bind_ok "$ER/void_receipt_1.json" && ok "record_void.sh writes tests void 1" || bad "record_void.sh void 1 (rc $rc)"
rc=0; TC_EVAL_ROOT="$W/er1" bash "$RVS" --repo "$F" --commit "$CF" --step 7 --code BUILD --reason "again" --command "make y" 2>/dev/null || rc=$?
[ "$rc" = 2 ] && [ ! -e "$ER/void_receipt_2.json" ] && ok "record_void.sh refuses a retry with a different command" || bad "record_void.sh retry differs (rc $rc)"
TC_EVAL_ROOT="$W/er1" bash "$RVS" --repo "$F" --commit "$CF" --step 7 --code BUILD --reason "again" --command "make x" 2>/dev/null || true
expect_refuse "evaluator void after two tests voids" OVERLAP -- run_sealed --dataset "$W/fds.json" --out "$ER" --work "$W/erw"
grep -q '"stage": "evaluation"' "$ER/void_receipt_3.json" && grep -q '"kind": "inconclusive_infra"' "$ER/final_receipt.json" &&
    ok "third void of EXP-001 across stages gives INCONCLUSIVE (INFRA)" || bad "global void counter"
rc=0; TC_EVAL_ROOT="$W/er1" bash "$RVS" --repo "$F" --commit "$CF" --step 7 --code BUILD --reason "again" --command "make x" 2>/dev/null || rc=$?
[ "$rc" = 2 ] && [ ! -e "$ER/void_receipt_4.json" ] && ok "record_void.sh refuses once EXP-001 has ended" || bad "record_void.sh after end (rc $rc)"
# S8 at the gate (Q12): a missing, unparsable or problem-reporting independent file is S8 FAIL, final, no retry.
s8_bad() { # s8_bad NAME CODE INDEP_FILE
    local b="$W/s8b_$1/$CF/run/bundle" rc=0
    run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$b" --work "$W/s8bw" 2>/dev/null || { bad "S8 $1 run"; return; }
    "$BIN" gate --bundle "$b" --independent "$3" >/dev/null 2>"$W/s8b.err" || rc=$?
    { [ "$rc" = 0 ] || [ "$rc" = 1 ]; } && grep -q '"S8": "FAIL"' "$b/final_receipt.json" && grep -q '"verdict": "FAIL"' "$b/final_receipt.json" &&
        { [ -z "$2" ] || grep -q "$2" "$b/final_receipt.json"; } && ok "S8: $1 -> FAIL, final" || bad "S8 $1: rc=$rc $(head -c 200 "$W/s8b.err")"
    expect_refuse "S8: $1, no second gate" EXISTS -- "$BIN" gate --bundle "$b" --independent "$3"
}
s8_bad missing_file INDEP_MISSING "$W/no_such_indep.json"
printf 'not json\n' >"$W/indep_garbage.json"
s8_bad unparsable "" "$W/indep_garbage.json"
sed 's/(primary)/(test copy)/' "$W/sealed/$CF/run/bundle/scorer_primary.json" | addp | sed 's/"problems": 0,/"problems": 1,/' >"$W/indep_p1.json"
s8_bad problems_reported "" "$W/indep_p1.json"

# Verdict rule at the gate (prereg section 5a): criteria forced in a pending receipt, S8 from a matching
# independent file with a different header (so not byte-identical).
verdict_case() { # verdict_case NAME S6 S7 S9 EXPECTED
    local b="$W/v_$1/$CF/run/bundle"
    run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$b" --work "$W/vw" 2>/dev/null || { bad "verdict case $1 run"; return; }
    sed -i -E "s/\"S6\": \"[A-Z_]+\"/\"S6\": \"$2\"/; s/\"S7\": \"[A-Z_]+\"/\"S7\": \"$3\"/; s/\"S9\": \"[A-Z_]+\"/\"S9\": \"$4\"/" "$b/final_receipt.pending.json"
    sed 's/(primary)/(test copy, same values)/' "$b/scorer_primary.json" | addp >"$W/indep_ok.json"
    "$BIN" gate --bundle "$b" --independent "$W/indep_ok.json" >/dev/null 2>&1 || true
    grep -q "\"verdict\": \"$5\"" "$b/final_receipt.json" && grep -q '"S8": "PASS"' "$b/final_receipt.json" &&
        ok "verdict rule: S6 $2, S7 $3, S9 $4 -> $5" || bad "verdict rule case $1"
}
verdict_case all_pass PASS PASS PASS PASS
verdict_case s6_straddle INCONCLUSIVE PASS PASS INCONCLUSIVE
verdict_case s9_straddle PASS PASS INCONCLUSIVE INCONCLUSIVE
verdict_case wrong_side_beats_straddle INCONCLUSIVE FAIL PASS FAIL

# After scoring, a failure is final (FAILURE_REPORTING.md section 2): a gate that cannot read a criterion from
# the pending receipt writes a terminal FAIL receipt, never a void one.
b="$W/v_terminal/$CF/run/bundle"
run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$b" --work "$W/vw" 2>/dev/null || bad "terminal case run"
sed -i -E 's/"S9": "[A-Z_]+"/"SX": "PASS"/' "$b/final_receipt.pending.json"
sed 's/(primary)/(test copy, same values)/' "$b/scorer_primary.json" | addp >"$W/indep_ok.json"
rc=0; "$BIN" gate --bundle "$b" --independent "$W/indep_ok.json" >/dev/null 2>&1 || rc=$?
[ "$rc" = 1 ] && grep -q '"kind": "terminal_fail"' "$b/final_receipt.json" && grep -q '"verdict": "FAIL"' "$b/final_receipt.json" &&
    [ ! -e "$b/void_receipt_1.json" ] && ok "failure after scoring is a terminal FAIL, not a void" || bad "terminal FAIL case: rc=$rc"

# NO_CRUMBS (EVALUATOR.md section 3, FAILURE_REPORTING.md section 2): a sealed group with no crumbs is final after
# scoring started. Group 2 empty -> S9 terminal FAIL (every other criterion NOT_REACHED); group 1 empty -> S6.
nocrumbs_case() { # nocrumbs_case NAME EMPTY_GROUP CRITERION
    local n="$1" eg="$2" crit="$3" s1 s2 d="$W/nc_$1" b="$W/nc_$1/$CF/run/bundle"
    s1="$(seed_of "$CF" "$FH" 1 0)" s2="$(seed_of "$CF" "$FH" 2 0)"
    mkdir -p "$d/seed-$s1/control" "$d/seed-$s2/control"
    cp "$FIX" "$d/seed-$s1/control/trace.ctr" && cp "$FIX" "$d/seed-$s2/control/trace.ctr"
    if [ "$eg" = 1 ]; then : >"$d/seed-$s1/control/trace.ctr"; else : >"$d/seed-$s2/control/trace.ctr"; fi
    bash "$F/calibration/scripts/make_dataset_manifest.sh" --dev "$d" "$s1" "$s2" "$FH" "$W/nc_$n.json" >/dev/null
    sed -i -E "s/\"split\": \"development\"/\"split\": \"sealed_test\"/; s/\"freeze_commit\": \"NONE\"/\"freeze_commit\": \"$CF\"/; s/\"released_utc\": \"NOT_SEALED\"/\"released_utc\": \"$REL\"/" "$W/nc_$n.json"
    local rc=0 i others=1
    run_sealed --dataset "$W/nc_$n.json" --overlap "$W/ov.json" --out "$b" --work "$W/ncw_$n" >"$W/last.out" 2>"$W/last.err" || rc=$?
    for i in 1 2 3 4 5 6 7 8 9; do
        [ "S$i" = "$crit" ] && continue
        grep -q "\"S$i\": \"NOT_REACHED\"" "$b/final_receipt.json" 2>/dev/null || others=0
    done
    if [ "$rc" = 1 ] && grep -q "FAILED .*: NO_CRUMBS:" "$W/last.err" && grep -q "\"$crit\": \"FAIL\"" "$b/final_receipt.json" &&
        grep -q "\"failed_criterion\": \"$crit\"" "$b/final_receipt.json" && grep -q '"code": "NO_CRUMBS"' "$b/final_receipt.json" &&
        grep -q '"kind": "terminal_fail"' "$b/final_receipt.json" && grep -q '"verdict": "FAIL"' "$b/final_receipt.json" &&
        grep -q '"EXP_001_COMPRESSION_BRIDGE": "FAIL"' "$b/final_receipt.json" && [ "$others" = 1 ] && [ ! -e "$b/void_receipt_1.json" ]; then
        ok "group $eg without crumbs: NO_CRUMBS is $crit terminal FAIL, final, not void"
    else bad "NO_CRUMBS group $eg: rc=$rc; stderr: $(head -c 300 "$W/last.err")"; fi
}
nocrumbs_case g2 2 S9
nocrumbs_case g1 1 S6

# S8 comparison contract (EVALUATOR.md section 6): exact int64 values, same key set, same schema and input
# digests, no repeated key; key order and spacing are free; a non-integer value never matches.
s8_case() { # s8_case NAME EXPECTED_S8 SED_SCRIPT
    local b="$W/s8_$1/$CF/run/bundle"
    run_sealed --dataset "$W/fds.json" --overlap "$W/ov.json" --out "$b" --work "$W/s8w" 2>/dev/null || { bad "S8 case $1 run"; return; }
    sed 's/(primary)/(test copy)/' "$b/scorer_primary.json" | addp | sed -E "$3" >"$W/indep_$1.json"
    "$BIN" gate --bundle "$b" --independent "$W/indep_$1.json" >/dev/null 2>&1 || true
    grep -q "\"S8\": \"$2\"" "$b/final_receipt.json" && ok "S8 contract: $1 -> $2" || bad "S8 contract case $1"
}
s8_case reordered_respaced PASS '1!G;h;$!d'
s8_case float_value FAIL '0,/"value": -?[0-9]+/s/("value": -?[0-9]+)/\1.0/'
s8_case other_profile_digest FAIL 's/("profile_sha256": ")[0-9a-f]/\10/'
s8_case repeated_key FAIL '0,/\{"key"/s/^( *\{"key": "[^"]*", "value": -?[0-9]+\})(,?)$/\1,\n\1\2/'


# Step 2 of the freeze: freeze_receipt.sh on the fake repo. First with the profile runtime_digest not filled
# (refused, check 4), then filled and committed as a new C_f (PASS), then with sealed data present (refused).
FR="$F/calibration/scripts/freeze_receipt.sh"
rc=0; TC_SEALED_ROOT="$W/nosealed" bash "$FR" "$CF" --no-fetch --out "$W/fr0.json" 2>"$W/fr0.err" || rc=$?
[ "$rc" = 2 ] && grep -q "check 4" "$W/fr0.err" && [ ! -e "$W/fr0.json" ] && ok "freeze receipt refused: runtime_digest not filled" || bad "freeze receipt check 4: $(cat "$W/fr0.err")"
RD="$(sed -n 's/^ *"runtime_digest": "\([0-9a-f]\{64\}\)".*/\1/p' "$FM")"
sed -i "s/^runtime_digest = \".*/runtime_digest = \"$RD\"/" "$FP"
refresh "$F"
commit_f
CF2="$(git -C "$F" rev-parse HEAD)"
git -C "$F" update-ref refs/remotes/origin/main "$CF2"
rc=0; TC_SEALED_ROOT="$W/nosealed" bash "$FR" "$CF2" --no-fetch --out "$W/fr1.json" 2>"$W/fr1.err" || rc=$?
[ "$rc" = 0 ] && grep -q '"TURING_PROFILE_V1_FROZEN": "PASS"' "$W/fr1.json" && grep -q "\"freeze_commit\": \"$CF2\"" "$W/fr1.json" &&
    ok "freeze receipt PASS names C_f" || bad "freeze receipt PASS: $(cat "$W/fr1.err")"
mkdir -p "$W/somesealed/$CF2"
rc=0; TC_SEALED_ROOT="$W/somesealed" bash "$FR" "$CF2" --no-fetch --out "$W/fr2.json" 2>"$W/fr2.err" || rc=$?
[ "$rc" = 2 ] && grep -q "check 7" "$W/fr2.err" && ok "freeze receipt refused: sealed data already exist" || bad "freeze receipt check 7"
# Q10 and Q1 at sealed generation: generate_sealed_data.sh refuses unless the PASS freeze receipt naming C_f is on
# origin/main; a failed generation is a void in the one counter; once three voids exist nothing starts.
GS="$F/calibration/scripts/generate_sealed_data.sh"
FH2="$(git -C "$F" show "$CF2:calibration/profiles/Turing-profile-v1.0.toml" | sha256sum | cut -c1-64)"
gen() { TC_SEALED_ROOT="$W/gsealed" TC_EVAL_ROOT="$W/er2" bash "$GS" --no-fetch --repo "$F" --commit "$CF2" --profile-digest "$FH2" >/dev/null 2>"$W/gen.err"; }
rc=0; gen || rc=$?
[ "$rc" = 2 ] && grep -q "freeze_receipt.json on origin/main" "$W/gen.err" && [ ! -e "$W/er2" ] && ok "generation refused: no freeze receipt on origin/main" || bad "Q10 no receipt: $(head -c 200 "$W/gen.err")"
mkdir -p "$F/calibration/experiments/EXP-001"
sed "s/$CF2/$CF/" "$W/fr1.json" >"$F/calibration/experiments/EXP-001/freeze_receipt.json"
commit_f && git -C "$F" update-ref refs/remotes/origin/main "$(git -C "$F" rev-parse HEAD)"
rc=0; gen || rc=$?
[ "$rc" = 2 ] && grep -q "names another freeze commit" "$W/gen.err" && [ ! -e "$W/er2" ] && ok "generation refused: freeze receipt names another commit" || bad "Q10 other commit: $(head -c 200 "$W/gen.err")"
cp "$W/fr1.json" "$F/calibration/experiments/EXP-001/freeze_receipt.json"
commit_f && git -C "$F" update-ref refs/remotes/origin/main "$(git -C "$F" rev-parse HEAD)"
E2="$W/er2/$CF2/run/bundle"
rc=0; gen || rc=$?
[ "$rc" = 2 ] && grep -q '"stage": "generation"' "$E2/void_receipt_1.json" && grep -q "\"freeze_commit\": \"$CF2\"," "$E2/void_receipt_1.json" &&
    ok "failed generation (learner build) is void 1 of EXP-001" || bad "generation void: rc=$rc $(tail -c 200 "$W/gen.err")"
TC_EVAL_ROOT="$W/er2" bash "$RVS" --repo "$F" --commit "$CF2" --step 7 --code TEST --reason "t" --command "make t" 2>/dev/null || true
gen || true
[ -e "$E2/void_receipt_3.json" ] && grep -q '"kind": "inconclusive_infra"' "$E2/final_receipt.json" && ok "generation + tests voids end EXP-001 at the third" || bad "generation global counter"
rc=0; gen || rc=$?
[ "$rc" = 2 ] && grep -q "has ended\|INCONCLUSIVE" "$W/gen.err" && [ ! -e "$E2/void_receipt_4.json" ] && ok "generation refused once EXP-001 has ended" || bad "generation after end"
sed "s/$CF/$CF2/g" "$W/fds.json" >"$W/fds_cf2.json"
expect_refuse "evaluator refused after three voids from other stages" ATTEMPTS -- run_sealed_raw --dataset "$W/fds_cf2.json" --overlap "$W/ov.json" --out "$E2" --work "$W/e2w"
# The worktree's EXP-001 directory is untouched by all of the above.
[ "$(exp_snap)" = "$EXP_BEFORE" ] && ok "calibration/experiments/EXP-001 unchanged" ||
    bad "calibration/experiments/EXP-001 changed"

echo "turing-cal-eval fail-closed tests: $pass passed, $fail failed"
[ "$fail" = 0 ]
