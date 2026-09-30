#!/bin/bash
# test_numeric_qualify.sh -- host-only tests of the Gate 5 (OMEGA-NUMERIC-0)
# qualifier in tests/run_numeric_gates.sh: physics pin checks, gate log
# checks and the immutable digest-named receipt writer. No GPU, no device.
# Everything is written under a temporary directory, never into evidence/.
# The "hardware" descriptors here are made up for the test and say so
# (UNIT_TEST_FAKE_NOT_HARDWARE); they are never from a device.
# Run: make test-numeric-qualify   (or run this file).
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
# shellcheck source=tests/run_numeric_gates.sh
. "$HERE/tests/run_numeric_gates.sh"
m19r_build_canon || { echo "FAIL: cannot build tools/json_canon.c"; exit 1; }
TMP=$(mktemp -d)
trap 'chmod -R u+w "$TMP"; rm -rf "$TMP" "$M19R_TMP"' EXIT
fails=0
ok() { echo "  [PASS] $1"; }
bad() { echo "  [FAIL] $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; [ -z "${SHOW_ERR:-}" ] || echo "        ($M19R_ERR)"; }
gitc() { git -C "$1" -c user.name=Test -c user.email=test@example.invalid "${@:2}"; }

echo "physics dependency comes from physics.lock"
phys=$TMP/physics
git init -q "$phys"
echo old > "$phys/README"
gitc "$phys" add README && gitc "$phys" commit -qm "no forge yet"
old=$(git -C "$phys" rev-parse HEAD)
omega=$TMP/omega
git init -q "$omega"
echo "$old" > "$omega/physics.lock"
gitc "$omega" add physics.lock && gitc "$omega" commit -qm lock
check "pinned commit without forge/ refused" '! num_check_physics "$omega" "$phys" "$old"'
check "  message names forge and the owner decision" '[[ $M19R_ERR == *"has no forge/forge_descriptor.h"*"owner decision"* ]]'
mkdir -p "$phys/forge" "$phys/nvrm" "$phys/m16"
for f in forge/forge_descriptor.h forge/forge_descriptor.c forge/forge_realize.c sha256_clean.c nvrm/nvrm.c m16/m16_native.c; do
    echo "/* stub */" > "$phys/$f"
done
gitc "$phys" add -A && gitc "$phys" commit -qm "forge"
new=$(git -C "$phys" rev-parse HEAD)
check "candidate differing from physics.lock refused" '! num_check_physics "$omega" "$phys" "$new"'
check "  message shows both commits" '[ "$M19R_ERR" = "physics.lock pins $old but --physics-candidate is $new" ]'
check "checkout not at the pinned commit refused" '! num_check_physics "$omega" "$phys" "$old"'
check "  message shows both commits" '[ "$M19R_ERR" = "physics checkout $phys is at $new but physics.lock pins $old" ]'
check "missing physics checkout refused" '! num_check_physics "$omega" "$TMP/nowhere" "$old"'
echo "$new" > "$omega/physics.lock"
gitc "$omega" commit -qam "move pin"
check "matching pin with forge/ accepted" 'num_check_physics "$omega" "$phys" "$new"'
echo dirty >> "$phys/README"
check "dirty physics tree refused" '! num_check_physics "$omega" "$phys" "$new"'
gitc "$phys" checkout -q README
echo "not-a-sha" > "$omega/physics.lock"
check "malformed physics.lock refused" '! num_check_physics "$omega" "$phys" "$new"'
echo "$new" > "$omega/physics.lock"

echo "gate log checks (comparison mode from the manifest, full launches, typed counts)"
fake_digest=$(printf 'UNIT_TEST_FAKE_NOT_HARDWARE' | sha256sum | cut -d' ' -f1)
LOG_BIN_SHA=$(printf 'unit-test-binary' | sha256sum | cut -d' ' -f1)
LOG_RUN_ID=unit-test-run
mode_of() { printf '%s\n' $NUM_OP_MANIFEST | awk -F: -v o="$1" '$1 == o { print $2 }'; }
# good_log -- what a passing chip run prints. The run line is the binary's
# own (run id + digest of /proc/self/exe); everything else is made up here.
good_log() {
    local id op mode c
    echo "OMEGA_NUMERIC_RUN_JSON:{\"run_id\":\"$LOG_RUN_ID\",\"binary_sha256\":\"$LOG_BIN_SHA\"}"
    for op in $NUM_ENCODED_OPS; do
        mode=$(mode_of "$op")
        echo "OMEGA_NUMERIC_REGISTRY_JSON:{\"op\":\"$op\",\"encoded\":true,\"compare\":\"$mode\",\"launches\":$([ "$op" = FFMA ] && echo 8 || echo 1)}"
    done
    for op in DIV SQRT EXP LOG; do
        echo "OMEGA_NUMERIC_REGISTRY_JSON:{\"op\":\"$op\",\"encoded\":false,\"compare\":\"BIT_EXACT\",\"launches\":1}"
    done
    for id in $NUM_EXPECTED_IDS; do echo "[PASS] $id"; done
    echo "OMEGA_NUMERIC_HWDESC_JSON:{\"source\":\"FORGE_PROBE\",\"descriptor_digest\":\"$fake_digest\",\"derived_alias\":\"UNIT_TEST_FAKE_NOT_HARDWARE\"}"
    for op in $NUM_ENCODED_OPS; do
        mode=$(mode_of "$op")
        case $mode in
            SEED_BOUND) echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"$op\",\"tier\":\"gb10\",\"compare\":\"SEED_BOUND\",\"n\":4096,\"checked\":3000,\"out_of_bound\":0,\"skipped\":1096}";;
            *) if [ "$op" = FFMA ]; then
                   for c in $NUM_FFMA_C; do
                       echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"FFMA\",\"tier\":\"gb10\",\"compare\":\"$mode\",\"c_bits\":\"$c\",\"n\":4096,\"checked\":4096,\"mismatches\":0}"
                   done
               elif [ "$op" = REDUCE_SUM ]; then
                   echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"REDUCE_SUM\",\"tier\":\"gb10\",\"compare\":\"$mode\",\"reduction_order\":\"$NUM_REDUCE_ORDER\",\"n\":4096,\"checked\":128,\"mismatches\":0}"
               else
                   echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"$op\",\"tier\":\"gb10\",\"compare\":\"$mode\",\"n\":4096,\"checked\":4096,\"mismatches\":0}"
               fi;;
        esac
        echo "OMEGA_NUMERIC_PARITY_JSON:{\"op\":\"$op\",\"tier\":\"cpu\",\"compare\":\"BIT_EXACT\",\"n\":4096,\"checked\":4096,\"mismatches\":0}"
    done
}
good_log > "$TMP/good.log"
# gb10 LOG SED -- the good log with one sed edit applied to the GB10 lines only.
gb10() { sed "/\"tier\":\"gb10\"/{
$2
}" "$TMP/good.log" > "$TMP/$1.log"
    cmp -s "$TMP/good.log" "$TMP/$1.log" && bad "fixture $1: the edit changed nothing"; }
check "complete passing log accepted" 'num_check_log "$TMP/good.log"'
check "  hardware digest read from the log" '[ "$NUM_HWDIGEST" = "$fake_digest" ]'
check "  22 GB10 parity lines kept (FFMA x8 + 14 ops)" '[ "$(printf "%s" "$NUM_PARITY" | jq length)" = 22 ]'
check "  run id and binary digest read from the run line" '[ "$NUM_LOG_RUN_ID" = "$LOG_RUN_ID" ] && [ "$NUM_LOG_BINARY_SHA" = "$LOG_BIN_SHA" ]'
sed 's/"source":"FORGE_PROBE"/"source":"FAKE_NON_HARDWARE_CPU_ONLY","fake":true/' "$TMP/good.log" > "$TMP/fake.log"
check "CPU-only fake descriptor refused" '! num_check_log "$TMP/fake.log"'
check "  as not a device probe" '[[ $M19R_ERR == "hardware descriptor is not a device probe"* ]]'
sed 's/"derived_alias"/"fake":true,"derived_alias"/' "$TMP/good.log" > "$TMP/fake2.log"
check "descriptor marked fake refused" '! num_check_log "$TMP/fake2.log"'
sed "s/$fake_digest/$(printf '0%.0s' {1..64})/" "$TMP/good.log" > "$TMP/zero.log"
check "all-zero descriptor digest refused" '! num_check_log "$TMP/zero.log"'
grep -v '"op":"SHFL_DOWN","tier":"gb10"' "$TMP/good.log" > "$TMP/noshfl.log"
check "missing GB10 parity line refused" '! num_check_log "$TMP/noshfl.log"'
check "  naming the op" '[ "$M19R_ERR" = "no GB10 parity line for SHFL_DOWN" ]'
gb10 onebit '/"op":"FFMA".*"c_bits":"0x7f800000"/s/"mismatches":0/"mismatches":1/'
check "one GB10 mismatch in one FFMA launch refused" '! num_check_log "$TMP/onebit.log"'
gb10 err '/"op":"FADD"/s/"n":4096.*}/"error":-4}/'
check "GB10 launch error refused" '! num_check_log "$TMP/err.log"'
gb10 mufu '/"op":"MUFU_RCP"/s/"out_of_bound":0/"out_of_bound":3/'
check "MUFU seed out of bound refused" '! num_check_log "$TMP/mufu.log"'
sed 's/^\[PASS\] FP32_SIMT_OPCODES_ENCODED/[FAIL] FP32_SIMT_OPCODES_ENCODED/' "$TMP/good.log" > "$TMP/fail.log"
check "a failed test refused" '! num_check_log "$TMP/fail.log"'
grep -v 'NEG_FTZ_DETECTED_AND_REJECTED' "$TMP/good.log" > "$TMP/miss.log"
check "missing expected test refused" '! num_check_log "$TMP/miss.log"'
{ cat "$TMP/good.log"; echo "[PASS] CPU_GB10_BIT_PARITY"; } > "$TMP/dup.log"
check "duplicated test refused" '! num_check_log "$TMP/dup.log"'
{ cat "$TMP/good.log"; echo "[SKIP] CPU_GB10_BIT_PARITY (needs GB10)"; } > "$TMP/skip.log"
check "any SKIP refused" '! num_check_log "$TMP/skip.log"'
{ cat "$TMP/good.log"; echo "[PASS] EXTRA_UNLISTED_TEST"; } > "$TMP/extra.log"
check "unlisted extra test refused" '! num_check_log "$TMP/extra.log"'
# Finding 5: the log does not choose its own comparison.
gb10 relabel '/"op":"FADD"/s/"compare":"BIT_EXACT","n":4096,"checked":4096,"mismatches":0/"compare":"SEED_BOUND","n":4096,"checked":4096,"mismatches":4096,"out_of_bound":0,"skipped":0/'
check "FADD relabelled SEED_BOUND with 4096 mismatches refused" '! num_check_log "$TMP/relabel.log"'
check "  comparison taken from the manifest" '[ "$M19R_ERR" = "GB10 parity for FADD: comparison \"SEED_BOUND\" but the manifest says BIT_EXACT" ]'
gb10 relabel2 '/"op":"FSEL"/s/"compare":"INT_EXACT"/"compare":"BIT_EXACT"/'
check "FSEL relabelled BIT_EXACT refused" '! num_check_log "$TMP/relabel2.log"'
gb10 strn '/"op":"FMUL"/s/"n":4096,"checked":4096/"n":"4096","checked":"4096"/'
check "string-typed counts refused" '! num_check_log "$TMP/strn.log"'
gb10 strm '/"op":"FMUL"/s/"mismatches":0/"mismatches":"0"/'
check "string-typed mismatches refused" '! num_check_log "$TMP/strm.log"'
gb10 frac '/"op":"FMUL"/s/"mismatches":0/"mismatches":0.5/'
check "fractional count refused" '! num_check_log "$TMP/frac.log"'
# Finding 6: full corpus, every launch, exact identities and multiplicities.
gb10 small '/"op":"FADD"/s/"n":4096,"checked":4096/"n":1,"checked":1/'
check "n=1 checked=1 refused" '! num_check_log "$TMP/small.log"'
gb10 partial '/"op":"FADD"/s/"checked":4096/"checked":4095/'
check "partial check (4095 of 4096) refused" '! num_check_log "$TMP/partial.log"'
gb10 seedsum '/"op":"MUFU_RSQ"/s/"skipped":1096/"skipped":1000/'
check "seed-bound checked+skipped != n refused" '! num_check_log "$TMP/seedsum.log"'
gb10 seedzero '/"op":"MUFU_RSQ"/s/"checked":3000,"out_of_bound":0,"skipped":1096/"checked":0,"out_of_bound":0,"skipped":4096/'
check "seed-bound with nothing checked refused" '! num_check_log "$TMP/seedzero.log"'
grep -v '"op":"FFMA","tier":"gb10".*"c_bits":"0x\(80000000\|00800000\|bf800000\|7fc00000\|7f800000\|00000003\|80800000\)"' "$TMP/good.log" > "$TMP/ffma1.log"
check "a single FFMA launch refused" '! num_check_log "$TMP/ffma1.log"'
check "  naming the missing launches" '[[ $M19R_ERR == "FFMA launches [\"0x3f800000\"], need exactly one per c in"* ]]'
gb10 ffmadup '/"op":"FFMA".*"c_bits":"0x80800000"/s/0x80800000/0x3f800000/'
check "a duplicated FFMA c (7 distinct of 8) refused" '! num_check_log "$TMP/ffmadup.log"'
{ cat "$TMP/good.log"; grep '"op":"FFMA","tier":"gb10".*"c_bits":"0x7fc00000"' "$TMP/good.log"; } > "$TMP/ffma9.log"
check "a ninth FFMA launch refused" '! num_check_log "$TMP/ffma9.log"'
{ cat "$TMP/good.log"; grep '"op":"FADD","tier":"gb10"' "$TMP/good.log"; } > "$TMP/fadd2.log"
check "a second FADD launch refused" '! num_check_log "$TMP/fadd2.log"'
gb10 cbits '/"op":"FADD"/s/"n":4096/"c_bits":"0x3f800000","n":4096/'
check "c_bits on a non-FFMA op refused" '! num_check_log "$TMP/cbits.log"'
gb10 stray '/"op":"FADD"/s/"op":"FADD"/"op":"DIV"/'
check "parity line for an op outside the manifest refused" '! num_check_log "$TMP/stray.log"'
sed '/OMEGA_NUMERIC_REGISTRY_JSON:{"op":"FADD"/s/"compare":"BIT_EXACT"/"compare":"SEED_BOUND"/' "$TMP/good.log" > "$TMP/reg1.log"
check "registry comparison differing from the manifest refused" '! num_check_log "$TMP/reg1.log"'
sed '/OMEGA_NUMERIC_REGISTRY_JSON:{"op":"FFMA"/s/"launches":8/"launches":1/' "$TMP/good.log" > "$TMP/reg2.log"
check "registry launch count differing from the manifest refused" '! num_check_log "$TMP/reg2.log"'
sed '/OMEGA_NUMERIC_REGISTRY_JSON:{"op":"DIV"/s/"encoded":false/"encoded":true/' "$TMP/good.log" > "$TMP/reg3.log"
check "registry encoding an op the manifest does not list refused" '! num_check_log "$TMP/reg3.log"'
# LDS_STS and REDUCE_SUM: one line each, full check, declared summation order.
grep -v '"op":"LDS_STS","tier":"gb10"' "$TMP/good.log" > "$TMP/nolds.log"
check "missing LDS_STS GB10 parity line refused" '! num_check_log "$TMP/nolds.log"'
check "  naming the op" '[ "$M19R_ERR" = "no GB10 parity line for LDS_STS" ]'
gb10 ldsbit '/"op":"LDS_STS"/s/"mismatches":0/"mismatches":1/'
check "one LDS_STS mismatch refused" '! num_check_log "$TMP/ldsbit.log"'
gb10 redbit '/"op":"REDUCE_SUM"/s/"mismatches":0/"mismatches":1/'
check "one REDUCE_SUM mismatch refused" '! num_check_log "$TMP/redbit.log"'
gb10 redall '/"op":"REDUCE_SUM"/s/"checked":128/"checked":4096/'
check "REDUCE_SUM claiming all 4096 lanes checked refused" '! num_check_log "$TMP/redall.log"'
check "  needs n/32 = 128 warp sums" '[ "$M19R_ERR" = "GB10 parity for REDUCE_SUM: checked 4096, need 128" ]'
gb10 redorder '/"op":"REDUCE_SUM"/s/"reduction_order":"[A-Z0-9_]*"/"reduction_order":"SEQUENTIAL_LANE_0_TO_31"/'
check "REDUCE_SUM with another summation order refused" '! num_check_log "$TMP/redorder.log"'
check "  naming both orders" '[[ $M19R_ERR == *"SEQUENTIAL_LANE_0_TO_31"*"$NUM_REDUCE_ORDER"* ]]'
gb10 rednoorder '/"op":"REDUCE_SUM"/s/"reduction_order":"[A-Z0-9_]*",//'
check "REDUCE_SUM without a declared order refused" '! num_check_log "$TMP/rednoorder.log"'
gb10 faddorder '/"op":"FADD"/s/"n":4096/"reduction_order":"PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1","n":4096/'
check "a summation order on an op that does not reduce refused" '! num_check_log "$TMP/faddorder.log"'
{ cat "$TMP/good.log"; grep '"op":"REDUCE_SUM","tier":"gb10"' "$TMP/good.log"; } > "$TMP/red2.log"
check "a second REDUCE_SUM launch refused" '! num_check_log "$TMP/red2.log"'
grep -v '^OMEGA_NUMERIC_RUN_JSON' "$TMP/good.log" > "$TMP/norun.log"
check "log without the binary's run line refused" '! num_check_log "$TMP/norun.log"'
sed 's/"binary_sha256":"[0-9a-f]*"/"binary_sha256":""/' "$TMP/good.log" > "$TMP/nobin.log"
check "run line without a binary digest refused" '! num_check_log "$TMP/nobin.log"'

echo "evidence directory (receipts never land in a candidate tree)"
keep_evd=${NUM_EVIDENCE_DIR:-}
M19R_PHYSICS=$phys
NUM_EVIDENCE_DIR=
check "default evidence directory accepted" 'num_check_evidence_dir "$HERE"'
check "  is ~/workspace/evidence-out/OMEGA-NUMERIC-0" '[ "$NUM_EVIDENCE_DIR" = "$(realpath -m "$HOME/workspace/evidence-out/OMEGA-NUMERIC-0")" ]'
check "  and lies outside this repository" '! num_under "$NUM_EVIDENCE_DIR" "$HERE"'
NUM_EVIDENCE_DIR=$HERE/evidence/OMEGA-NUMERIC-0
check "evidence directory inside this repository refused" '! num_check_evidence_dir "$HERE"'
check "  saying receipts must live outside the trees" '[[ $M19R_ERR == *"receipts must live outside the candidate trees" ]]'
NUM_EVIDENCE_DIR=$omega/evidence/OMEGA-NUMERIC-0
check "evidence directory inside the omega candidate refused" '! num_check_evidence_dir "$omega"'
NUM_EVIDENCE_DIR=$omega
check "the omega tree itself refused" '! num_check_evidence_dir "$omega"'
NUM_EVIDENCE_DIR=$phys/receipts
check "evidence directory inside the physics checkout refused" '! num_check_evidence_dir "$omega"'
NUM_EVIDENCE_DIR=$TMP/evd/../omega/receipts
check "  also when reached through .." '! num_check_evidence_dir "$omega"'
ln -s "$omega" "$TMP/omega-link"
NUM_EVIDENCE_DIR=$TMP/omega-link/receipts
check "  also when reached through a symlink" '! num_check_evidence_dir "$omega"'
NUM_EVIDENCE_DIR=$omega-sibling
check "a sibling whose name only starts like the tree accepted" 'num_check_evidence_dir "$omega"'
check "script refuses an in-tree --evidence-dir with exit 2, before any run" '"$HERE/tests/run_numeric_gates.sh" --omega-candidate x --physics-candidate y --evidence-dir "$HERE/evidence/OMEGA-NUMERIC-0" >/dev/null 2>&1; [ $? = 2 ] && [ ! -e "$HERE/evidence/OMEGA-NUMERIC-0" ]'
NUM_EVIDENCE_DIR=$keep_evd

cpu_bin=$HERE/build/test_omega_numeric_cpu
if [ -x "$cpu_bin" ]; then
    "$cpu_bin" > "$TMP/cpu.log" 2>&1
    check "real CPU-only run log refused (no receipt without the chip)" '! num_check_log "$TMP/cpu.log"'
    # Drift: the test binary's own registry and FFMA launches equal the manifest.
    m19r_tagged_json "$TMP/cpu.log" OMEGA_NUMERIC_REGISTRY_JSON | jq -sc . > "$TMP/cpu.reg"
    check "binary registry equals the shell manifest" '[ -z "$(jq -r --argjson man "$(num_manifest_json)" "$NUM_JQ_REGISTRY" "$TMP/cpu.reg")" ]'
    check "binary FFMA c values equal the shell manifest" '[ "$(m19r_tagged_json "$TMP/cpu.log" OMEGA_NUMERIC_PARITY_JSON | jq -sc "[.[] | select(.op == \"FFMA\" and .tier == \"cpu\") | .c_bits] | sort")" = "$(num_manifest_json | jq -c ".ffma_c | sort")" ]'
    check "binary corpus size equals the shell manifest" '[ "$(m19r_tagged_json "$TMP/cpu.log" OMEGA_NUMERIC_PARITY_JSON | jq -s "[.[] | select(.tier == \"cpu\" and .op == \"FADD\") | .n] | .[0]")" = "$NUM_CORPUS_N" ]'
else
    bad "build/test_omega_numeric_cpu missing (run make test-numeric-cpu first)"
fi

echo "execution record: exit status, binary and log digests, run id"
# A stand-in gate binary: prints STUB_LOG, then the same run line the real
# binary prints (run id from OMEGA_NUMERIC_RUN_ID, digest of /proc/self/exe),
# then exits with STUB_RC. It proves the plumbing, never a chip result.
cat > "$TMP/stub.c" <<'STUB'
#include <stdio.h>
#include <stdlib.h>
#include "sha256.h"
int main(void) {
    const char *log = getenv("STUB_LOG"), *rc = getenv("STUB_RC"), *id = getenv("OMEGA_NUMERIC_RUN_ID");
    char line[4096], hex[65] = "";
    uint8_t buf[65536], dig[SHA256_DIGEST_SIZE];
    size_t got;
    sha256_ctx ctx;
    FILE *f = log ? fopen(log, "r") : NULL;
    if (f) { while (fgets(line, sizeof line, f)) fputs(line, stdout); fclose(f); }
    f = fopen("/proc/self/exe", "rb");
    if (f) {
        sha256_init(&ctx);
        while ((got = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&ctx, buf, got);
        sha256_final(&ctx, dig);
        for (int i = 0; i < SHA256_DIGEST_SIZE; i++) snprintf(hex + 2 * i, 3, "%02x", dig[i]);
        fclose(f);
    }
    printf("OMEGA_NUMERIC_RUN_JSON:{\"run_id\":\"%s\",\"binary_sha256\":\"%s\"}\n", id ? id : "", hex);
    return rc ? atoi(rc) : 0;
}
STUB
grep -v '^OMEGA_NUMERIC_RUN_JSON' "$TMP/good.log" > "$TMP/stub_body.log"
mkdir -p "$omega/evidence"
echo '{"historical":true}' > "$omega/evidence/m19r_gate5_omega_numeric_evidence.json"
gitc "$omega" add -A && gitc "$omega" commit -qm "historical evidence"
NUM_OMEGA_CAND=$(git -C "$omega" rev-parse HEAD) NUM_PHYSICS_CAND=$new M19R_PHYSICS=$phys
NUM_TS=$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ) NUM_RECORD=0
EVD=$TMP/evidence-out/OMEGA-NUMERIC-0 NUM_EVIDENCE_DIR=$TMP/evidence-out/OMEGA-NUMERIC-0
export STUB_LOG=$TMP/stub_body.log
# fresh_run RC -- a new run directory, build the stand-in, run it like num_qualify.
fresh_run() {
    NUM_RUN_ID=unit-$1-$RANDOM NUM_RUN_DIR=$TMP/run-$NUM_RUN_ID
    mkdir -p "$NUM_RUN_DIR"
    gcc -std=gnu11 -O2 -I"$HERE/src" -o "$NUM_RUN_DIR/test_omega_numeric" "$TMP/stub.c" "$HERE/src/sha256.c" || return 1
    NUM_BINARY_SHA=$(m19r_sha_file "$NUM_RUN_DIR/test_omega_numeric")
    NUM_RUN_COMMIT=$NUM_OMEGA_CAND
    STUB_RC=$1 num_execute "$omega" "$NUM_RUN_DIR/test_omega_numeric"
}
# Finding 7: a nonzero exit status disqualifies, whatever the log says.
check "binary printing only passing records but exiting 3 refused" '! fresh_run 3'
check "  exit status recorded" '[ "$NUM_EXEC_RC" = 3 ] && [ "$(cat "$NUM_RUN_DIR/gate5.status")" = 3 ]'
check "  its log passes every log check on its own" 'num_check_log "$NUM_RUN_DIR/gate5.log"'
check "  yet no receipt from it" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log" && [ ! -e "$NUM_RUN_DIR/receipt-preview.json" ]'
NUM_EXEC_RC=0
check "  nor with the exit status variable forged to 0 (status file says 3)" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'

check "binary exiting 0 accepted by the executor" 'fresh_run 0'
check "passing run makes a PASS preview" 'num_receipt "$omega" "$NUM_RUN_DIR/gate5.log" && [ -f "$NUM_RUN_DIR/receipt-preview.json" ]'
pv=$NUM_RUN_DIR/receipt-preview.json
check "  status PASS, exit status 0, digests bound" '[ "$(jq -r "(.status == \"PASS\") and (.gate_binary_exit_status == 0) and (.candidate_binary_sha256 == \"$NUM_BINARY_SHA\") and (.gate_log_sha256 == \"$NUM_LOG_SHA\") and (.run_id == \"$NUM_RUN_ID\")" "$pv")" = true ]'
check "  clean flags from git status, commits from git" '[ "$(jq -r "(.candidate_trees_clean == {\"omega\":true,\"physics\":true}) and (.run_git_commit == \"$NUM_OMEGA_CAND\") and (.candidate_git_commit == .run_git_commit)" "$pv")" = true ]'
check "  observed counts 22/22/0, historical untouched, predecessor recorded" '[ "$(jq -c "[.observed_test_count,.observed_pass_count,.observed_fail_count]" "$pv")" = "[22,22,0]" ] && [ "$(cat "$omega/evidence/m19r_gate5_omega_numeric_evidence.json")" = "{\"historical\":true}" ] && [ "$(jq -r ".predecessor_historical_gate5_sha256 | length" "$pv")" = 64 ]'
check "  no permanent receipt without --record" '[ ! -e "$EVD" ] && [ ! -e "$omega/evidence/OMEGA-NUMERIC-0" ]'
# Finding 4: nothing is taken on trust at receipt time.
cp "$TMP/good.log" "$TMP/fabricated.log"
check "fabricated log outside the run refused" '! num_receipt "$omega" "$TMP/fabricated.log"'
cp "$NUM_RUN_DIR/gate5.log" "$TMP/keep.log"
cp "$TMP/good.log" "$NUM_RUN_DIR/gate5.log"
check "log replaced after the run refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
check "  as changed since the run" '[[ $M19R_ERR == "gate log changed since the run"* ]]'
cp "$TMP/keep.log" "$NUM_RUN_DIR/gate5.log"
keep_sha=$NUM_BINARY_SHA
NUM_BINARY_SHA=$(printf 'other' | sha256sum | cut -d' ' -f1)
check "64-hex binary digest not matching the built binary refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
NUM_BINARY_SHA=$keep_sha
cp "$NUM_RUN_DIR/test_omega_numeric" "$TMP/keep.bin"
printf 'x' >> "$NUM_RUN_DIR/test_omega_numeric"
check "binary changed after the run refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
cp "$TMP/keep.bin" "$NUM_RUN_DIR/test_omega_numeric"
# A log whose run line names another binary, with every stored digest made to agree.
sed "s/\"binary_sha256\":\"[0-9a-f]*\"/\"binary_sha256\":\"$LOG_BIN_SHA\"/" "$TMP/keep.log" > "$NUM_RUN_DIR/gate5.log"
keep_log_sha=$NUM_LOG_SHA NUM_LOG_SHA=$(m19r_sha_file "$NUM_RUN_DIR/gate5.log")
check "log produced by a different binary refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
check "  naming both digests" '[ "$M19R_ERR" = "log was produced by binary $LOG_BIN_SHA, not $NUM_BINARY_SHA" ]'
cp "$TMP/keep.log" "$NUM_RUN_DIR/gate5.log"; NUM_LOG_SHA=$keep_log_sha
keep_id=$NUM_RUN_ID NUM_RUN_ID=unit-other-run
mv "$NUM_RUN_DIR" "$TMP/run-$NUM_RUN_ID"; keep_dir=$NUM_RUN_DIR NUM_RUN_DIR=$TMP/run-$NUM_RUN_ID
check "log from another run id refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
mv "$NUM_RUN_DIR" "$keep_dir"; NUM_RUN_DIR=$keep_dir NUM_RUN_ID=$keep_id
NUM_RUN_COMMIT=0000000000000000000000000000000000000000
check "run commit differing from candidate refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
NUM_RUN_COMMIT=$NUM_OMEGA_CAND
keep_cand=$NUM_OMEGA_CAND NUM_OMEGA_CAND=$(printf '%040d' 1)
check "omega candidate not at HEAD refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
NUM_OMEGA_CAND=$keep_cand
echo dirty > "$omega/untracked"
check "dirty omega tree at receipt time refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
rm -f "$omega/untracked"
echo dirty >> "$phys/README"
check "dirty physics tree at receipt time refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
gitc "$phys" checkout -q README
check "num_tree_clean reads real git status" '[ "$(num_tree_clean "$omega")" = true ] && echo x > "$omega/untracked" && [ "$(num_tree_clean "$omega")" = false ] && rm -f "$omega/untracked"'
check "recheck passes again once restored" 'num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
NUM_RECORD=1
check "PASS receipt into a repository other than this script's refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"'
check "  and nothing written" '[ ! -e "$EVD" ] && [ ! -e "$omega/evidence/OMEGA-NUMERIC-0" ]'
keep_m19r=$M19R_OMEGA M19R_OMEGA=$omega
check "PASS receipt recorded (script repository = this tree)" 'num_receipt "$omega" "$NUM_RUN_DIR/gate5.log" && [ -f "$EVD/$NUM_DIGEST.json" ] && [ "$NUM_PERMANENT" = "$(realpath -m "$EVD")/$NUM_DIGEST.json" ]'
prec=$EVD/$NUM_DIGEST.json
check "  outside the tree, which stays clean" '[ "$(num_tree_clean "$omega")" = true ] && [ "$(num_tree_clean "$phys")" = true ] && [ ! -e "$omega/evidence/OMEGA-NUMERIC-0" ]'
check "  binds run id, both commits, clean flags and binary digest" '[ "$(jq -r "(.status == \"PASS\") and (.run_id == \"$NUM_RUN_ID\") and (.candidate_git_commit == \"$NUM_OMEGA_CAND\") and (.physics_candidate_git_commit == \"$NUM_PHYSICS_CAND\") and (.candidate_trees_clean == {\"omega\":true,\"physics\":true}) and (.candidate_binary_sha256 == \"$NUM_BINARY_SHA\")" "$prec")" = true ]'
check "  named by its digest, mode 0444" '[ "$(jq "del(.receipt_digest)" "$prec" | "$JSON_CANON" --sha256)" = "$NUM_DIGEST" ] && [ "$(stat -c %a "$prec")" = 444 ]'
check "  a second record of the same PASS refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log" && [ "$(ls "$EVD" | wc -l)" = 1 ]'
NUM_EVIDENCE_DIR=$omega/evidence/OMEGA-NUMERIC-0
check "PASS receipt into an in-tree evidence directory refused" '! num_receipt "$omega" "$NUM_RUN_DIR/gate5.log" && [ ! -e "$omega/evidence/OMEGA-NUMERIC-0" ]'
NUM_EVIDENCE_DIR=$EVD M19R_OMEGA=$keep_m19r
rm -f "$prec" 2>/dev/null || { chmod u+w "$EVD"; rm -f "$prec"; }

echo "append-only receipts"
NUM_EXEC_RC=3
# No git exclude needed: receipts live outside the tree, so it stays clean
# and a repeat receipt is byte-identical.
check "failed run writes a FAIL receipt" 'num_fail_receipt "$omega" "gate binary exited with status 3"'
frec=$EVD/$NUM_DIGEST.json
check "  named by its digest, mode 0444" '[ -f "$frec" ] && [ "$(jq "del(.receipt_digest)" "$frec" | "$JSON_CANON" --sha256)" = "$NUM_DIGEST" ] && [ "$(stat -c %a "$frec")" = 444 ]'
check "  omega tree still clean after recording (reported true in the receipt)" '[ "$(num_tree_clean "$omega")" = true ] && [ "$(jq -r .candidate_trees_clean.omega "$frec")" = true ] && [ "$(jq -r ".run_id == \"$NUM_RUN_ID\"" "$frec")" = true ]'
check "  status FAIL with the reason and exit status" '[ "$(jq -r "(.status == \"FAIL\") and (.error == \"gate binary exited with status 3\") and (.gate_binary_exit_status == 3)" "$frec")" = true ]'
check "  no preview left claiming a pass" '[ "$(jq -r .status "$NUM_RUN_DIR/receipt-preview.json")" = PASS ] || [ ! -e "$NUM_RUN_DIR/receipt-preview.json" ]'
orig=$(sha256sum < "$frec")
check "same FAIL receipt again refused (never overwritten)" '! num_fail_receipt "$omega" "gate binary exited with status 3"'
check "  because the file exists" '[[ $M19R_ERR == *"$NUM_DIGEST"* ]]'
check "  original bytes unchanged" '[ "$(sha256sum < "$frec")" = "$orig" ]'
check "a different failure appends a second receipt" 'num_fail_receipt "$omega" "another reason" && [ "$(ls "$EVD" | wc -l)" = 2 ] && [ "$(num_tree_clean "$omega")" = true ]'
urec=$TMP/immut/unit.json
check "immutable writer: first write" 'printf "{\"unit_test\":true,\"status\":\"PASS\"}" | m19r_write_immutable_receipt "$urec"'
check "immutable writer: overwrite refused, bytes kept" '! printf "{\"unit_test\":true,\"status\":\"FAIL\"}" | m19r_write_immutable_receipt "$urec" && [ "$(jq -r .status "$urec")" = PASS ]'

if [ "$fails" -ne 0 ]; then
    echo "Gate 5 qualifier tests: $fails FAILED"
    exit 1
fi
echo "Gate 5 qualifier tests: all passed"
