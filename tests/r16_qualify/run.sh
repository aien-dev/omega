#!/bin/bash
# Self-test for tools/r16_qualify.sh. No chip, no make target, no build: the script runs in
# dry mode (R16_QUALIFY_DRY=1) inside a scratch git repo, reading fixture logs.
# Proves: a dirty tree is refused before anything runs; a gate with no check prints
# NOT_RUN (never PASS); a failing check makes the script exit non-zero; observed fields
# are computed, not literals; G1/G2 follow every count, repo head and result in the
# inventory JSON; the G7 R15 receipt check fails on each broken field. Each check has a
# passing fixture first and then a counterexample that must fail.
# Usage: bash tests/r16_qualify/run.sh
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
SCR=$(mktemp -d "${TMPDIR:-/tmp}/r16qual.XXXXXX") || exit 2
trap 'rm -rf "$SCR"' EXIT INT TERM
FAILS=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }

W=$SCR/w
mkdir -p "$W/tools"
cp "$HERE/tools/r16_qualify.sh" "$HERE/tools/aienos_lock_source.sh" "$W/tools/"
G6FILES="src/omega_accelerator_world.c src/runtime/rx_generation.c src/omega_world_gates.c tools/omegatool.c src/runtime/rx_native_bind.c src/omega_evidence.c src/omega_verify.c src/runtime/rx_resident_gpu.c src/runtime/rx_seq_reference.c src/runtime/rx_world.c src/runtime/rx_living.c src/runtime/rx_operator.c tests/runtime/rx_r13_living.c"
for p in $G6FILES; do mkdir -p "$W/$(dirname "$p")"; cp "$HERE/$p" "$W/$p"; done
printf 'build/\n' > "$W/.gitignore"
g() { git -C "$W" -c user.name=r16 -c user.email=r16@invalid -c commit.gpgsign=false "$@"; }
g init -q && g add -A && g commit -qm fixture || exit 2
Q() { env R16_QUALIFY_DRY=1 "$@" bash "$W/tools/r16_qualify.sh"; }

# 1. dirty tree refused before anything runs
echo x > "$W/stray.txt"
out=$(Q 2>&1); rc=$?
check "dirty tree: exit 2" '[ $rc = 2 ]'
check "dirty tree: says REFUSED" 'echo "$out" | grep -q "^REFUSED: working tree is dirty"'
check "dirty tree: nothing created (no build dir)" '[ ! -e "$W/build" ]'
check "dirty tree: no gate lines printed" '! echo "$out" | grep -q "R16-G"'
rm -f "$W/stray.txt"
echo y >> "$W/.gitignore"
out=$(Q 2>&1); rc=$?
check "modified tracked file: exit 2" '[ $rc = 2 ]'
g checkout -q -- .gitignore

# 2. wrong expected commit refused
out=$(Q R16_EXPECT_COMMIT=0000000000000000000000000000000000000000 2>&1); rc=$?
check "unexpected HEAD: exit 2" '[ $rc = 2 ]'

# 3. output under tracked evidence/ refused
out=$(Q R16_OUT_DIR="$W/evidence/R16/raw" 2>&1); rc=$?
check "output under evidence/: exit 2" '[ $rc = 2 ]'

# 4. no fixtures: every gate NOT_RUN, nothing PASS, exit 3
out=$(Q R16_RUN_ID=none 2>&1); rc=$?
R=$W/build/r16-raw/none/DRY-RUN-receipt.json
check "no fixtures: exit 3 (nothing ran)" '[ $rc = 3 ]'
check "no fixtures: G6 NOT_RUN (every item implemented, no test ran)" 'echo "$out" | grep -q "    G6=NOT_RUN" && ! echo "$out" | grep -q "G6=MISSING_IMPLEMENTATION"'
check "no fixtures: summary all NOT_RUN" 'echo "$out" | grep -q "^Gates: G1=NOT_RUN G2=NOT_RUN G3=NOT_RUN G4=NOT_RUN G5=NOT_RUN G6=NOT_RUN G7=NOT_RUN G8=NOT_RUN$"'
check "no fixtures: receipt gate NOT_RUN" 'grep -q "\"gate\": \"NOT_RUN\"" "$R"'
check "no fixtures: receipt has no PASS value" '! grep -q "\"PASS\"" "$R"'
check "no fixtures: silicon_observed false (computed)" 'grep -q "\"silicon_observed\": false" "$R"'
check "no fixtures: tree_dirty false and candidate_bound true observed" 'grep -q "\"tree_dirty\": false" "$R" && grep -q "\"candidate_bound\": true" "$R"'
check "receipt marks schema revision 2" 'grep -q "\"schema_revision\": 2" "$R"'
check "receipt keeps schema name" 'grep -q "\"schema\": \"AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1\"" "$R"'
check "tree still clean after run" '[ -z "$(g status --porcelain)" ]'

# 5. fixture logs: G4 good, G5 log lacks the required line -> FAIL -> exit 1
F=$W/build/r16-raw/fx
mkdir -p "$F"
echo "R16 gate: R16_G4_LEGACY_REFUSED=PASS" > "$F/r16_negative.log"; echo 0 > "$F/r16_negative.log.rc"
echo "R16 gate: R16_G4_GUARDS_LOAD_BEARING=PASS" > "$F/r16_negative_mutants.log"; echo 0 > "$F/r16_negative_mutants.log.rc"
echo "surface: something else" > "$F/r16_surface.log"; echo 0 > "$F/r16_surface.log.rc"
out=$(Q R16_RUN_ID=fx 2>&1); rc=$?
R=$W/build/r16-raw/fx/DRY-RUN-receipt.json
check "failing grep: exit non-zero (1)" '[ $rc = 1 ]'
check "failing grep: G5 FAIL" 'echo "$out" | grep -q "G5=FAIL"'
check "failing grep: G4 PASS from its own checks" 'echo "$out" | grep -q "G4=PASS"'
check "failing grep: receipt gate FAIL" 'grep -q "\"gate\": \"FAIL\"" "$R"'

# 6. target exit status counts even when the pass line is present
echo 1 > "$F/r16_surface.log.rc"; echo "R16 gate: R16_G5_SURFACE=PASS" > "$F/r16_surface.log"
out=$(Q R16_RUN_ID=fx 2>&1); rc=$?
check "bad exit status with pass line: G5 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G5=FAIL"'
echo 0 > "$F/r16_surface.log.rc"
out=$(Q R16_RUN_ID=fx 2>&1); rc=$?
check "good G4/G5 fixtures: those gates PASS" 'echo "$out" | grep -q "G4=PASS" && echo "$out" | grep -q "G5=PASS"'
check "all-good G4/G5 still exit 3 (G6 tests did not run)" '[ $rc = 3 ] && echo "$out" | grep -q "G6=NOT_RUN"'

# 6b. R11 that skipped its living run (load > 2) exits 0 with "failures 0": NOT_RUN, never PASS
printf 'checks 400 failures 0\n' > "$F/r11_aien.log"; echo 0 > "$F/r11_aien.log.rc"
out=$(Q R16_RUN_ID=fx 2>&1)
check "R11 rules-only log with living run: PASS" 'echo "$out" | grep -q "^    R11: PASS$"'
printf '[-] SKIPPED-LOADED: living run not started: load average 15.85 is above 2\n[-] living run not exercised: SKIPPED-LOADED: load average 15.85 is above 2\nchecks 400 failures 0\n' > "$F/r11_aien.log"
out=$(Q R16_RUN_ID=fx 2>&1)
check "R11 living run skipped under load: NOT_RUN" 'echo "$out" | grep -q "^    R11: NOT_RUN$"'
rm -f "$F/r11_aien.log" "$F/r11_aien.log.rc"


# 7. G1/G2 from the inventory JSON. A good fixture passes first (so each failure below is
# caused by the one value changed), then every G1/G2 check is broken on its own.
I=$W/build/r16-raw/inv
mkdir -p "$I"
# inv_fixture [key=value ...]: writes inventory.json in the tool's layout with all counts 0,
# result PASS and five recorded heads; a key=value pair overrides one field
# (head_<repo>=<sha> overrides one repo head; an empty value records no head).
inv_fixture() {
    local unclassified=0 question_rows=0 bad_class=0 a_reachable=0 stale_rows=0 map_errors=0
    local skipped_repos=0 result=PASS kv k v r
    local head_omega=1111111111111111111111111111111111111111
    local head_aien_sovereign_core=2222222222222222222222222222222222222222
    local head_aegis_runtime=3333333333333333333333333333333333333333
    local head_aienos=4444444444444444444444444444444444444444
    local head_physics=5555555555555555555555555555555555555555
    for kv in "$@"; do k=${kv%%=*}; v=${kv#*=}; k=${k//-/_}; printf -v "$k" '%s' "$v"; done
    {
        printf '{\n  "schema": "AIEN_R16_LOOP_INVENTORY_V1",\n  "map": "spec/r16-orchestrator-retirement-map.md",\n  "repos": [\n'
        for r in omega aien-sovereign-core aegis-runtime aienos physics; do
            k=head_${r//-/_}
            printf '    {"name": "%s", "path": "/x/%s", "head": "%s", "map_sha": "", "status": "SCANNED", "sites_by_class": {"A": 0}}%s\n' \
                "$r" "$r" "${!k}" "$([ "$r" = physics ] || echo ,)"
        done
        printf '  ],\n  "sites": [\n  ],\n  "manual_rows": 5,\n  "sites_total": 328,\n'
        printf '  "unclassified": %s,\n  "question_rows": %s,\n  "bad_class": %s,\n  "a_reachable": %s,\n' \
            "$unclassified" "$question_rows" "$bad_class" "$a_reachable"
        printf '  "stale_rows": %s,\n  "map_errors": %s,\n  "skipped_repos": %s,\n  "class_n_sites": 0,\n  "result": "%s"\n}\n' \
            "$stale_rows" "$map_errors" "$skipped_repos" "$result"
    } > "$I/inventory.json"
}
inv_logs() {   # inv_logs <inventory rc> <self-test rc> <self-test last line>
    echo "R16 loop inventory: sites=328 -> fixture" > "$I/r16_inventory.log"; echo "$1" > "$I/r16_inventory.log.rc"
    echo "$3" > "$I/test_r16_inventory.log"; echo "$2" > "$I/test_r16_inventory.log.rc"
}
inv_logs 0 0 "R16 inventory self-test: PASS"
inv_fixture
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
R=$I/DRY-RUN-receipt.json
check "good inventory: G1 PASS and G2 PASS" 'echo "$out" | grep -q "G1=PASS G2=PASS"'
check "good inventory: no FAIL, exit 3 (other gates NOT_RUN)" '[ $rc = 3 ]'
check "good inventory: counts read from the JSON (0 and 0)" 'grep -q "\"remaining_unclassified_semantic_loop_count\": 0" "$R" && grep -q "\"remaining_central_loop_count\": 0" "$R"'
check "good inventory: repo heads read from the JSON" 'grep -q "\"aien_sovereign_core_commit\": \"2222222222222222222222222222222222222222\"" "$R" && grep -q "\"aegis_runtime_commit\": \"3333333333333333333333333333333333333333\"" "$R"'
for k in unclassified question_rows bad_class stale_rows map_errors skipped_repos; do
    inv_fixture "$k=1"
    out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
    check "bad inventory $k=1: G1 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=FAIL"'
done
inv_fixture unclassified=7
out=$(Q R16_RUN_ID=inv 2>&1)
check "bad inventory unclassified=7: count carried into receipt" 'grep -q "\"remaining_unclassified_semantic_loop_count\": 7" "$R"'
for r in omega aien-sovereign-core aegis-runtime aienos physics; do
    inv_fixture "head_$r="
    out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
    check "inventory with no head for $r: G1 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=FAIL"'
done
inv_fixture a_reachable=1
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "bad inventory a_reachable=1: G2 FAIL (G1 PASS), exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=PASS G2=FAIL"'
check "bad inventory a_reachable=1: count carried into receipt" 'grep -q "\"remaining_central_loop_count\": 1" "$R"'
inv_fixture result=FAIL
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory result FAIL: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_fixture; inv_logs 1 0 "R16 inventory self-test: PASS"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory target exit 1: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_logs 0 0 "R16 inventory self-test: 1 FAILED"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory self-test log lacks pass line: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_logs 0 1 "R16 inventory self-test: PASS"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory self-test exit 1 with pass line: G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G2=FAIL"'
inv_logs 0 0 "R16 inventory self-test: PASS"; rm -f "$I/inventory.json"
out=$(Q R16_RUN_ID=inv 2>&1); rc=$?
check "inventory ran but wrote no JSON: G1 FAIL and G2 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G1=FAIL G2=FAIL"'

# 8. G7 R15 acceptance receipt (R16_R15_RECEIPT). A good receipt (named by its own
# SHA-256, bound to this HEAD) passes first; then each check is broken on its own.
CAND=$(g rev-parse HEAD)
RD=$SCR/r15
mkdir -p "$RD"
# r15_receipt [field=value ...]: writes a receipt named by its own SHA-256, prints its path.
r15_receipt() {
    local outcome=PASS candidate_commit=$CAND candidate_bound=true tree_dirty=false silicon_observed=true kv
    for kv in "$@"; do printf -v "${kv%%=*}" '%s' "${kv#*=}"; done
    printf '{\n  "schema": "AIEN_RX_R15_FIXTURE",\n  "outcome": "%s",\n  "candidate_commit": "%s",\n  "candidate_bound": %s,\n  "tree_dirty": %s,\n  "silicon_observed": %s\n}\n' \
        "$outcome" "$candidate_commit" "$candidate_bound" "$tree_dirty" "$silicon_observed" > "$RD/tmp.json"
    local s; s=$(sha256sum "$RD/tmp.json" | cut -d' ' -f1)
    mv "$RD/tmp.json" "$RD/$s.json"
    echo "$RD/$s.json"
}
GOOD=$(r15_receipt)
out=$(Q R16_RUN_ID=r15 R16_R15_RECEIPT="$GOOD" 2>&1); rc=$?
R=$W/build/r16-raw/r15/DRY-RUN-receipt.json
check "good R15 receipt: acceptance PASS" 'echo "$out" | grep -q "R15 acceptance receipt: PASS"'
check "good R15 receipt: field true, exit 3 (ladder NOT_RUN)" '[ $rc = 3 ] && grep -q "\"r15_acceptance_still_passing\": true" "$R"'
r15_bad() {   # r15_bad <label> <receipt path>
    out=$(Q R16_RUN_ID=r15 R16_R15_RECEIPT="$2" 2>&1); rc=$?
    check "R15 receipt $1: acceptance FAIL, G7 FAIL, exit 1" \
        '[ $rc = 1 ] && echo "$out" | grep -q "R15 acceptance receipt: FAIL" && echo "$out" | grep -q "G7=FAIL" && grep -q "\"r15_acceptance_still_passing\": false" "$R"'
}
cp "$GOOD" "$RD/not-its-sha.json"
r15_bad "not named by its SHA-256" "$RD/not-its-sha.json"
r15_bad "missing file" "$RD/absent.json"
: > "$RD/empty.json"
r15_bad "empty file" "$RD/empty.json"
r15_bad "for another commit" "$(r15_receipt candidate_commit=0000000000000000000000000000000000000000)"
r15_bad "outcome FAIL" "$(r15_receipt outcome=FAIL)"
r15_bad "candidate_bound false" "$(r15_receipt candidate_bound=false)"
r15_bad "tree_dirty true" "$(r15_receipt tree_dirty=true)"
r15_bad "silicon_observed false" "$(r15_receipt silicon_observed=false)"

# 8b. G6 protected things kept. Presence (symbol defined) and operation (test ran and
# passed in this run) are separate. All good first, then one control at a time.
G=$W/build/r16-raw/g6
mkdir -p "$G"
# A stand-in for the pinned capability-root tree (the pinned-commit lookup of the lock file): a tiny repo holding the file
# the script looks for, and a lock file in the fixture repo naming its commit.
LK=$SCR/lockrepo
mkdir -p "$LK/native/capability"
printf 'int aienos_cap_validate(const void *view)\n{ return 0; }\nint aienos_cap_mint(void *admin)\n{ return 0; }\n' > "$LK/native/capability/aienos_capability.c"
git -C "$LK" init -q && git -C "$LK" add -A && git -C "$LK" -c user.name=r16 -c user.email=r16@invalid -c commit.gpgsign=false commit -qm lock
git -C "$LK" rev-parse HEAD > "$W/aienos.lock"
g add -A; g commit -qm "lock file"
export AIENOS_LOCK_REPO=$LK
# Operator receipts (G2): each seat's test writes one; the script copies it into the raw
# directory and checks it against the production binary of the run. Fixture binaries in
# build/ (git-ignored); templates filled with the current HEAD and binary digest by G6Q.
mkdir -p "$W/build"
printf 'host production binary fixture\n' > "$W/build/rx_r13_living_host"
printf 'silicon production binary fixture\n' > "$W/build/rx_r13_living_silicon"
# op_tmpl <seat> [sed expression...]: the receipt template of one seat, then edits.
op_tmpl() {
    local seat=$1 gate=HOST_PASS_NON_SILICON e; shift
    [ "$seat" = silicon ] && gate=PASS
    printf '{\n  "schema": "AIEN_R16_G6_OPERATOR_CONTROL_V1",\n  "seat": "%s",\n  "candidate_commit": "@HEAD@",\n  "tree_dirty": false,\n  "binary": "rx_r13_living_%s",\n  "binary_sha256": "@SHA@",\n  "checks": 90,\n  "failures": 0,\n  "skipped": 0,\n  "skip_reasons": [],\n  "gate": {"R16_G6_OPERATOR": "%s"}\n}\n' \
        "$seat" "$seat" "$gate" > "$G/op_$seat.tmpl"
    for e in "$@"; do sed -i "$e" "$G/op_$seat.tmpl"; done
}
op_render() {
    local s
    for s in host silicon; do
        rm -f "$G/operator_${s}_receipt.json"
        [ -f "$G/op_$s.tmpl" ] || continue
        sed -e "s/@HEAD@/$(g rev-parse HEAD)/" -e "s/@SHA@/$(sha256sum < "$W/build/rx_r13_living_$s" | cut -d' ' -f1)/" \
            "$G/op_$s.tmpl" > "$G/operator_${s}_receipt.json"
    done
}
# g6log <file> <pattern> [rc]: fixture log + exit status for one exercising test
g6log() { printf '%s\n' "$2" > "$G/$1"; echo "${3:-0}" > "$G/$1.rc"; }
g6_good_logs() {
    g6log m19_world.log "STAGE 1 / PERSISTENT WORLD QUALIFICATION: 18 / 18 M19 GATES PASSED"
    g6log r9_barrier.log "generation barrier kept a single coherent generation through every injected stop"
    g6log r10_omega.log "checks 40 failures 0"
    g6log r7_native.log "native authority matched the linux oracle"
    g6log r12_host.log "checks 40 failures 0"
    g6log r12_silicon.log "checks 40 failures 0 silicon 1"
    g6log rx_emergency.log "RX_EMERGENCY_STOP: PASS (0 failures; host CPU, real caproot authority)
RX_EMERGENCY_MUTANTS: PASS"
    g6log r13_host.log "R13 gate: R13_LIVING_SYSTEM=HOST_PASS_NON_SILICON"
    g6log r14_host.log "R14 gate: R14_LIVING_RECOVERY=HOST_PASS_NON_SILICON"
    g6log r14_silicon.log "R14 gate: R14_LIVING_RECOVERY=PASS"
    g6log r15_parity_host.log "SEQ_SEMANTIC_PARITY=PASS"
    g6log r15_parity_silicon.log "SEQ_SEMANTIC_PARITY=PASS"
    g6log r15_receipt.log "r15 receipt test: 0 failure(s)"
    g6log r16_operator_host.log "R16 operator: 120 checks, 0 failures, 0 skipped (host, phases: startup control restart kill)
R16 operator gate: R16_G6_OPERATOR=HOST_PASS_NON_SILICON"
    g6log r16_operator_mutants.log "RX_OPERATOR_MUTANTS: 14 mutants, 0 failures, 0 skipped
RX_OPERATOR_MUTANTS: PASS"
    g6log r16_operator_silicon.log "R16 operator: 100 checks, 0 failures, 0 skipped (silicon, phases: startup control restart kill)
R16 operator gate: R16_G6_OPERATOR=PASS"
    op_tmpl host; op_tmpl silicon
}
G6Q() { op_render; Q R16_RUN_ID=g6 2>&1; }
R=$G/DRY-RUN-receipt.json
g6_item() { sed -n 's/^.*"item": "'"$1"'", "status": "\([A-Z_]*\)".*$/\1/p' "$R"; }
g6_good_logs
out=$(G6Q); rc=$?
check "G6 all good: every item implemented and operated, G6 PASS" 'echo "$out" | grep -q "    G6=PASS"'
check "G6 all good: twelve items PASS, operator emergency controls among them" '[ "$(grep -c "\"item\": \"[a-z0-9_]*\", \"status\": \"PASS\"" "$R")" = 12 ] && [ "$(g6_item operator_emergency_controls_passing)" = PASS ]'
check "G6 all good: protected_surfaces_kept six presence flags true" '[ "$(grep -c "_present\": true" "$R")" = 6 ]'
check "G6 all good: no FAIL anywhere and G6 alone does not make the run pass (exit 3)" '[ $rc = 3 ] && ! echo "$out" | grep -q "G6=FAIL"'
check "G6 all good: receipt names file and symbol per item" 'grep -q "\"file\": \"src/runtime/rx_generation.c\", \"symbol\": \"rx_gen_recover\", \"source\": \"omega_tree\", \"defined\": true" "$R"'
check "G6 operated checks cite the test and its status" 'grep -q "{\"name\": \"R9\", \"target\": \"test-r9\", \"seat\": \"host\", \"status\": \"PASS\"}" "$R"'

# presence without operation: symbols all defined, tests never ran -> NOT_RUN, never PASS
rm -f "$G"/*.log "$G"/*.rc
out=$(G6Q)
check "G6 presence alone (no test ran): item NOT_RUN, G6 not PASS" '[ "$(g6_item recovery_path_present)" = NOT_RUN ] && ! echo "$out" | grep -q "G6=PASS"'
g6_good_logs

# symbol removed / commented out / reduced to a prototype -> MISSING_IMPLEMENTATION
RG=$W/src/runtime/rx_generation.c
g6_mut() {   # g6_mut <label> <sed expression>: mutate the fixture source, commit, run, report, revert
    sed -i "$2" "$RG"; g add -A; g commit -qm "mutation: $1"
    out=$(G6Q); rc=$?
}
g6_unmut() { g reset -q --hard HEAD~1; }
g6_mut "symbol removed" 's/rx_gen_recover/rx_gen_rec0ver/'
check "G6 symbol removed: recovery_path MISSING_IMPLEMENTATION" '[ "$(g6_item recovery_path_present)" = MISSING_IMPLEMENTATION ]'
check "G6 symbol removed: r9 item (same routine) also MISSING_IMPLEMENTATION, r14 (fn_restore) unaffected" '[ "$(g6_item r9_crash_recovery_passing)" = MISSING_IMPLEMENTATION ] && [ "$(g6_item r14_recovery_paths_passing)" = PASS ]'
check "G6 symbol removed: G6 is not PASS even though every test passed" 'echo "$out" | grep -q "G6=MISSING_IMPLEMENTATION" && [ $rc = 3 ]'
check "G6 symbol removed: receipt flag is not true" 'grep -q "\"recovery_path_present\": \"MISSING_IMPLEMENTATION\"" "$R"'
g6_unmut
g6_mut "definition commented out (disabled)" 's|^int rx_gen_recover(|// int rx_gen_recover(|'
check "G6 definition commented out: MISSING_IMPLEMENTATION" '[ "$(g6_item recovery_path_present)" = MISSING_IMPLEMENTATION ]'
g6_unmut
g6_mut "definition reduced to a prototype" 's|^int rx_gen_recover(const char \*dir, RxRecoveryRecord \*out) {|int rx_gen_recover(const char *dir, RxRecoveryRecord *out);|'
check "G6 prototype only: MISSING_IMPLEMENTATION" '[ "$(g6_item recovery_path_present)" = MISSING_IMPLEMENTATION ]'
g6_unmut
out=$(G6Q)
check "G6 mutations reverted: recovery_path back to PASS" '[ "$(g6_item recovery_path_present)" = PASS ]'

# failing tests -> FAIL
g6log r9_barrier.log "generation barrier kept a single coherent generation through every injected stop" 1
out=$(G6Q); rc=$?
check "G6 test exits 1 with its pass line present: item FAIL, G6 FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G6=FAIL" && [ "$(g6_item recovery_path_present)" = FAIL ] && [ "$(g6_item generation_mechanism_present)" = FAIL ]'
g6log r9_barrier.log "2 generation-barrier failures"
out=$(G6Q); rc=$?
check "G6 fake log reporting failures (no pass line): FAIL, exit 1" '[ $rc = 1 ] && echo "$out" | grep -q "G6=FAIL"'
g6log r9_barrier.log "generation barrier kept a single coherent generation through every injected stop"
g6log r12_silicon.log "checks 40 failures 3 silicon 1" 1
out=$(G6Q)
check "G6 a failing silicon seat-loss run: G6 FAIL" 'echo "$out" | grep -q "G6=FAIL" && [ "$(g6_item r12_seat_loss_handling_passing)" = FAIL ]'
g6log r12_silicon.log "checks 40 failures 0 silicon 1"
g6log m19_world.log "STAGE 1 / PERSISTENT WORLD QUALIFICATION: 17 / 18 M19 GATES PASSED
  [FAIL] OMEGA_ACCEL_RESIDENT_FAULT_RECOVERY_PASS : x" 0
out=$(G6Q)
check "G6 M19 log with a [FAIL] gate line (exit 0): known_good_fallback FAIL" '[ "$(g6_item known_good_fallback_present)" = FAIL ]'
g6log m19_world.log "STAGE 1 / PERSISTENT WORLD QUALIFICATION: 18 / 18 M19 GATES PASSED"

# operator emergency controls: operated only by execution of the production program,
# host and silicon apart; the old string-presence check can no longer pass it.
g6log r16_operator_silicon.log "R16 operator gate: R16_G6_OPERATOR=HOST_PASS_NON_SILICON"
out=$(G6Q)
check "G6 operator: a host result in the silicon slot is not a silicon pass: item FAIL" '[ "$(g6_item operator_emergency_controls_passing)" = FAIL ]'
rm -f "$G/r16_operator_silicon.log" "$G/r16_operator_silicon.log.rc"
out=$(G6Q)
check "G6 operator: host test and mutants passed, no silicon receipt: item NOT_RUN, G6 not PASS" '[ "$(g6_item operator_emergency_controls_passing)" = NOT_RUN ] && ! echo "$out" | grep -q "G6=PASS"'
g6log r16_operator_silicon.log "[SKIPPED] unauthorized: peer of another uid (sudo -n unavailable)
R16 operator gate: R16_G6_OPERATOR=PASS"
out=$(G6Q)
check "G6 operator: silicon log with a SKIPPED case and the pass line: NOT_RUN, never PASS" '[ "$(g6_item operator_emergency_controls_passing)" = NOT_RUN ]'
g6log r16_operator_silicon.log "[-] stop: authorized stop accepted, durable
R16 operator gate: R16_G6_OPERATOR=FAIL" 1
out=$(G6Q)
check "G6 operator: failed silicon run: item FAIL, G6 FAIL" '[ "$(g6_item operator_emergency_controls_passing)" = FAIL ] && echo "$out" | grep -q "G6=FAIL"'
g6log r16_operator_silicon.log "R16 operator gate: R16_G6_OPERATOR=PASS"
g6log r16_operator_host.log "R16 operator gate: R16_G6_OPERATOR=FAIL" 1
out=$(G6Q)
check "G6 operator: failed host run with a passing silicon log: item FAIL" '[ "$(g6_item operator_emergency_controls_passing)" = FAIL ]'
g6log r16_operator_host.log "R16 operator gate: R16_G6_OPERATOR=PASS"
out=$(G6Q)
check "G6 operator: a host log carrying the silicon pass line is not the host verdict: FAIL" '[ "$(g6_item operator_emergency_controls_passing)" = FAIL ]'
g6log r16_operator_host.log "R16 operator gate: R16_G6_OPERATOR=HOST_PASS_NON_SILICON"
g6log r16_operator_mutants.log "ok   O1 wiring missing: killed
FAIL O5 success reply without a state transition: SURVIVED (exit 0)
RX_OPERATOR_MUTANTS: FAIL" 1
out=$(G6Q)
check "G6 operator: a surviving mutant: item FAIL" '[ "$(g6_item operator_emergency_controls_passing)" = FAIL ]'
g6log r16_operator_mutants.log "RX_OPERATOR_MUTANTS: PASS"
RL=$W/tests/runtime/rx_r13_living.c
sed -i 's|^static int operator_open(Rig \*r) {|/* static int operator_open(Rig *r) { */ static int operator_0pen(Rig *r) {|' "$RL"; g add -A; g commit -qm "mutation: entry point only in a comment"
out=$(G6Q)
check "G6 operator: production wiring present only in a comment (string still in the file), every log passing: MISSING_IMPLEMENTATION" '[ "$(g6_item operator_emergency_controls_passing)" = MISSING_IMPLEMENTATION ] && grep -q "operator_open(Rig" "$RL" && ! echo "$out" | grep -q "G6=PASS"'
g6_unmut
out=$(G6Q)
check "G6 operator: reverted, all logs passing: PASS" '[ "$(g6_item operator_emergency_controls_passing)" = PASS ]'
# G2: the operator receipt. Good receipts first, then one broken field at a time.
op_item() { g6_item operator_emergency_controls_passing; }
op_test() { jq -r ".g6_items[] | select(.item==\"operator_emergency_controls_passing\") | .tests[] | select(.name==\"$1\") | .$2" "$R"; }
check "G6 operator receipts: both copied into the raw directory and covered by its digest" \
    'grep -q " operator_host_receipt.json$" "$G/SHA256SUMS" && grep -q " operator_silicon_receipt.json$" "$G/SHA256SUMS" && [ "$(op_item)" = PASS ]'
check "G6 operator: tests labelled by seat (host test host, silicon test silicon)" \
    '[ "$(op_test R16_OP_HOST seat)" = host ] && [ "$(op_test R16_OP_MUTANTS seat)" = host ] && [ "$(op_test R16_OP_SILICON seat)" = silicon ] && [ "$(jq -r .operator_control.silicon.seat "$R")" = silicon ]'
op_bad() {   # op_bad <label> <seat> <sed expression>: that receipt broken: item FAIL
    OPSEAT=$2; op_tmpl "$2" "$3"; out=$(G6Q)
    check "G6 operator receipt $1: item FAIL" '[ "$(op_item)" = FAIL ] && echo "$out" | grep -q "operator $OPSEAT receipt: FAIL"'
    op_tmpl "$2"
}
op_bad "binary digest is not the binary built in the run (host)" host 's/@SHA@/0000000000000000000000000000000000000000000000000000000000000000/'
op_bad "binary digest is not the binary built in the run (silicon)" silicon 's/@SHA@/1111111111111111111111111111111111111111111111111111111111111111/'
op_bad "written by a mutant run (binary prog)" host 's/"binary": "rx_r13_living_host"/"binary": "prog"/'
op_bad "another candidate commit" silicon 's/@HEAD@/0123456789abcdef0123456789abcdef01234567/'
op_bad "dirty tree" host 's/"tree_dirty": false/"tree_dirty": true/'
op_bad "a skipped case" silicon 's/"skipped": 0/"skipped": 1/'
op_bad "a failure" host 's/"failures": 0/"failures": 2/'
op_bad "the host gate in the silicon receipt" silicon 's/"R16_G6_OPERATOR": "PASS"/"R16_G6_OPERATOR": "HOST_PASS_NON_SILICON"/'
op_bad "the silicon seat in the host slot" host 's/"seat": "host"/"seat": "silicon"/'
rm -f "$G/op_host.tmpl"; out=$(G6Q)
check "G6 operator: host log passing but no host receipt: item FAIL" '[ "$(op_item)" = FAIL ]'
op_tmpl host
printf 'changed after the test\n' >> "$W/build/rx_r13_living_silicon"
sed -i "s/@SHA@/$(sha256sum < "$W/build/rx_r13_living_silicon" | cut -d' ' -f1)/" "$G/op_silicon.tmpl"
printf 'rebuilt\n' >> "$W/build/rx_r13_living_silicon"; out=$(G6Q)
check "G6 operator: silicon binary rebuilt after its test: item FAIL" '[ "$(op_item)" = FAIL ]'
op_tmpl silicon; out=$(G6Q)
check "G6 operator: receipts restored: PASS" '[ "$(op_item)" = PASS ]'
# G7: a run that only skipped a case (gate NOT_RUN, exit 0) is NOT_RUN, never FAIL.
g6log r16_operator_host.log "[SKIPPED] unauthorized: a peer of another uid needs sudo -n (no passwordless sudo here)
R16 operator: 85 checks, 0 failures, 1 skipped (host, phases: startup control restart kill)
R16 operator gate: R16_G6_OPERATOR=NOT_RUN (1 SKIPPED)"
op_tmpl host 's/"skipped": 0/"skipped": 1/' 's/"HOST_PASS_NON_SILICON"/"NOT_RUN (1 SKIPPED)"/'; out=$(G6Q)
check "G6 operator: host run without sudo (NOT_RUN gate, exit 0): test NOT_RUN, not FAIL" '[ "$(op_test R16_OP_HOST status)" = NOT_RUN ] && [ "$(op_item)" = NOT_RUN ]'
g6log r16_operator_host.log "[SKIPPED] unauthorized: a peer of another uid needs sudo -n (no passwordless sudo here)
R16 operator gate: R16_G6_OPERATOR=NOT_RUN (1 SKIPPED)" 1
out=$(G6Q)
check "G6 operator: the same NOT_RUN line with exit 1: FAIL" '[ "$(op_test R16_OP_HOST status)" = FAIL ]'
g6log r16_operator_host.log "R16 operator: 120 checks, 0 failures, 0 skipped (host, phases: startup control restart kill)
R16 operator gate: R16_G6_OPERATOR=HOST_PASS_NON_SILICON"
op_tmpl host
g6log r16_operator_mutants.log "SKIPPED O7 peer uid check removed: the test could not run the case that kills it
RX_OPERATOR_MUTANTS: NOT_RUN (SKIPPED cases)" 3
out=$(G6Q)
check "G6 operator: mutants NOT_RUN (exit 3): test NOT_RUN, not FAIL" '[ "$(op_test R16_OP_MUTANTS status)" = NOT_RUN ]'
g6log r16_operator_mutants.log "RX_OPERATOR_MUTANTS: PASS"
# G3: silicon_observed needs every silicon run, the operator silicon test included.
g6log r16_authpath_host.log "R16 gate: R16_G3_AUTHPATH=HOST_PASS_NON_SILICON"
g6log r16_authpath_silicon.log "R16 gate: R16_G3_AUTHPATH=PASS"
g6log r13_silicon.log "R13 gate: R13_LIVING_SYSTEM=PASS"
out=$(G6Q)
check "G3 silicon_observed: every silicon run passed, the operator silicon run included: true" 'grep -q "\"silicon_observed\": true" "$R"'
rm -f "$G/r16_operator_silicon.log" "$G/r16_operator_silicon.log.rc"; out=$(G6Q)
check "G3 silicon_observed: the five other silicon runs passed, operator silicon not run: false" 'grep -q "\"silicon_observed\": false" "$R"'
g6log r16_operator_silicon.log "R16 operator gate: R16_G6_OPERATOR=PASS"
op_tmpl silicon 's/@SHA@/2222222222222222222222222222222222222222222222222222222222222222/'; out=$(G6Q)
check "G3 silicon_observed: operator silicon log passing but its receipt unbound: false" 'grep -q "\"silicon_observed\": false" "$R"'
op_tmpl silicon
rm -f "$G"/r16_authpath_*.log* "$G/r13_silicon.log" "$G/r13_silicon.log.rc"

# skipped tests are never PASS
g6log r9_barrier.log "[-] living run not exercised: SKIPPED-LOADED
generation barrier kept a single coherent generation through every injected stop"
out=$(G6Q); rc=$?
check "G6 skipped test with pass line and exit 0: item NOT_RUN, never PASS" '[ "$(g6_item recovery_path_present)" = NOT_RUN ] && ! echo "$out" | grep -q "G6=PASS" && [ $rc = 3 ]'
g6log r9_barrier.log "generation barrier kept a single coherent generation through every injected stop"

# a test that never ran (log absent) -> NOT_RUN for exactly the items that need it
rm -f "$G/r10_omega.log" "$G/r10_omega.log.rc"
out=$(G6Q)
check "G6 one test absent: only its item NOT_RUN" '[ "$(g6_item r10_verifier_passing)" = NOT_RUN ] && [ "$(g6_item r9_crash_recovery_passing)" = PASS ]'
# M19 must show N / N with N nonzero: 17 / 18 (exit 0, no [FAIL] line) and 0 / 0 are not PASS
g6log m19_world.log "STAGE 1 / PERSISTENT WORLD QUALIFICATION: 17 / 18 M19 GATES PASSED"
out=$(G6Q)
check "G6 M19 log 17 / 18 with exit 0: known_good_fallback FAIL" '[ "$(g6_item known_good_fallback_present)" = FAIL ]'
g6log m19_world.log "STAGE 1 / PERSISTENT WORLD QUALIFICATION: 0 / 0 M19 GATES PASSED"
out=$(G6Q)
check "G6 M19 log 0 / 0: known_good_fallback FAIL" '[ "$(g6_item known_good_fallback_present)" = FAIL ]'
g6log m19_world.log "M19 GATES PASSED"
out=$(G6Q)
check "G6 M19 log with only the substring: known_good_fallback FAIL" '[ "$(g6_item known_good_fallback_present)" = FAIL ]'
g6log m19_world.log "STAGE 1 / PERSISTENT WORLD QUALIFICATION: 18 / 18 M19 GATES PASSED"

# second and third items' presence checks: another omega file, and the pinned capability-root source
AW=$W/src/omega_accelerator_world.c
sed -i 's/omega_world_drain(/omega_world_dra1n(/' "$AW"; g add -A; g commit -qm "mutation: omega_world_drain removed"
out=$(G6Q)
check "G6 omega_world_drain removed (other file): known_good_fallback MISSING_IMPLEMENTATION" '[ "$(g6_item known_good_fallback_present)" = MISSING_IMPLEMENTATION ] && ! echo "$out" | grep -q "G6=PASS"'
g reset -q --hard HEAD~1
sed -i 's/aienos_cap_mint(/aienos_cap_m1nt(/' "$LK/native/capability/aienos_capability.c"
git -C "$LK" -c user.name=r16 -c user.email=r16@invalid -c commit.gpgsign=false commit -qam mut
git -C "$LK" rev-parse HEAD > "$W/aienos.lock"; g add -A; g commit -qm "lock mutated"
out=$(G6Q)
check "G6 capability-root symbol removed from the pinned tree: trusted_capability_root MISSING_IMPLEMENTATION" '[ "$(g6_item trusted_capability_root_present)" = MISSING_IMPLEMENTATION ]'
g reset -q --hard HEAD~1
out=$(Q R16_RUN_ID=g6 AIENOS_LOCK_REPO=/nonexistent 2>&1)
check "G6 pinned tree unreadable: trusted_capability_root NOT_RUN (not MISSING, not PASS)" '[ "$(g6_item trusted_capability_root_present)" = NOT_RUN ]'
out=$(G6Q)
check "G6 pinned tree readable again: trusted_capability_root PASS" '[ "$(g6_item trusted_capability_root_present)" = PASS ]'

# 8c. capability-root source identity (the locked commit, never a directory). Lock A lacks
# aienos_cap_mint; every place that has it must not turn the item PASS.
GOODLOCK=$(cat "$W/aienos.lock")
lkg() { git -C "$LK" -c user.name=r16 -c user.email=r16@invalid -c commit.gpgsign=false "$@"; }
lkg checkout -q -b lacks-mint "$GOODLOCK"
sed -i 's/aienos_cap_mint(/aienos_cap_other(/' "$LK/native/capability/aienos_capability.c"; lkg commit -qam "lock A: no mint"
ALOCK=$(git -C "$LK" rev-parse HEAD); lkg checkout -q -
printf '%s\n' "$ALOCK" > "$W/aienos.lock"; g add -A; g commit -qm "lock A"
UNREL=$SCR/unrelated; mkdir -p "$UNREL/native/capability"
printf 'int aienos_cap_validate(const void *v)\n{ return 0; }\nint aienos_cap_mint(void *a)\n{ return 0; }\n' > "$UNREL/native/capability/aienos_capability.c"
out=$(Q R16_RUN_ID=g6 AIENOS_R7_DIR="$UNREL" 2>&1)
check "G6 lock A + unrelated dir with plausible functions: MISSING_IMPLEMENTATION (dir not read)" '[ "$(g6_item trusted_capability_root_present)" = MISSING_IMPLEMENTATION ]'
out=$(Q R16_RUN_ID=g6 AIENOS_R7_DIR="$UNREL" AIENOS_LOCK_REPO=/nonexistent 2>&1)
check "G6 lock A unavailable + unrelated dir supplied: NOT_RUN (not PASS)" '[ "$(g6_item trusted_capability_root_present)" = NOT_RUN ]'
check "G6 unavailable: receipt g6_lock_source status unavailable" '[ "$(jq -r .g6_lock_source.status "$R")" = unavailable ]'
CLONE=$SCR/clean-other; git clone -q "$LK" "$CLONE"; git -C "$CLONE" checkout -q "$GOODLOCK"
out=$(Q R16_RUN_ID=g6 AIENOS_R7_DIR="$CLONE" 2>&1)
check "G6 lock A + clean checkout of a different commit that has mint: MISSING_IMPLEMENTATION" '[ "$(g6_item trusted_capability_root_present)" = MISSING_IMPLEMENTATION ]'
out=$(Q R16_RUN_ID=g6 AIENOS_R7_DIR="$CLONE" AIENOS_LOCK_REPO=/nonexistent 2>&1)
check "G6 lock A, no lock repo, other-commit checkout whose repo holds A: MISSING_IMPLEMENTATION (A read, not the files)" '[ "$(g6_item trusted_capability_root_present)" = MISSING_IMPLEMENTATION ]'
for c in "$ALOCK" "${ALOCK:0:7}"; do
    mkdir -p "$W/build/aienos-authority/$c/native/capability"
    cp "$UNREL/native/capability/aienos_capability.c" "$W/build/aienos-authority/$c/native/capability/"
done
out=$(Q R16_RUN_ID=g6 2>&1)
check "G6 lock A + modified cache dirs (full and short name) with mint: MISSING_IMPLEMENTATION" '[ "$(g6_item trusted_capability_root_present)" = MISSING_IMPLEMENTATION ]'
rm -rf "$W/build/aienos-authority"
lkg replace "$ALOCK" "$GOODLOCK"
out=$(Q R16_RUN_ID=g6 2>&1)
check "G6 lock A with a git replace ref onto a commit that has mint: MISSING_IMPLEMENTATION" '[ "$(g6_item trusted_capability_root_present)" = MISSING_IMPLEMENTATION ]'
lkg replace -d "$ALOCK" >/dev/null
printf '%s\n' "${GOODLOCK:0:7}" > "$W/aienos.lock"; g add -A; g commit -qm "short lock"
out=$(Q R16_RUN_ID=g6 2>&1)
check "G6 abbreviated aienos.lock: NOT_RUN, g6_lock_source malformed_lock" '[ "$(g6_item trusted_capability_root_present)" = NOT_RUN ] && [ "$(jq -r .g6_lock_source.status "$R")" = malformed_lock ]'
printf '%s\n' "$GOODLOCK" > "$W/aienos.lock"; g add -A; g commit -qm "good lock again"
out=$(G6Q)
check "G6 valid locked source: PASS" '[ "$(g6_item trusted_capability_root_present)" = PASS ]'
check "G6 valid: receipt records the full lock, its tree and the blob read" '[ "$(jq -r .g6_lock_source.aienos_lock "$R")" = "$GOODLOCK" ] && [ "$(jq -r .g6_lock_source.commit_tree "$R")" = "$(git -C "$LK" rev-parse "$GOODLOCK^{tree}")" ] && [ "$(jq -r "[.g6_items[] | select(.item==\"trusted_capability_root_present\") | .implementation[] | select(.source==\"aienos_lock_commit\") | .blob] | unique | .[0]" "$R")" = "$(git -C "$LK" rev-parse "$GOODLOCK:native/capability/aienos_capability.c")" ]'
out=$(Q R16_RUN_ID=g6 AIENOS_CAP_LIB=/tmp/prebuilt.a 2>&1); rc=$?
check "prebuilt AIENOS_CAP_LIB override refused (exit 2)" '[ $rc = 2 ] && echo "$out" | grep -q "no source proof"'

# receipt is valid JSON; each item says plainly what its mapping is (no shared boilerplate)
check "G6 receipt is valid JSON (jq)" 'jq -e . "$R" >/dev/null'
check "G6 receipt: twelve items, twelve different basis texts" '[ "$(jq "[.g6_items[].basis] | unique | length" "$R")" = 12 ] && [ "$(jq ".g6_items | length" "$R")" = 12 ]'
check "G6 receipt: proxy mappings say PROXY, emergency item says it is operated only by execution" '[ "$(jq "[.g6_items[] | select(.basis | startswith(\"PROXY\"))] | length" "$R")" -ge 5 ] && jq -e ".g6_items[] | select(.item==\"operator_emergency_controls_passing\") | .basis | contains(\"operated only by execution\")" "$R" >/dev/null'
check "G6 receipt: seat kill hook is not an item symbol" '! jq -e ".g6_items[].implementation[] | select(.symbol==\"rx_gpu_seat_kill\")" "$R" >/dev/null'
check "G6 emergency item: runtime and production wiring defined, RX_EMERGENCY cited" 'jq -e ".g6_items[] | select(.item==\"operator_emergency_controls_passing\") | (.implementation[] | select(.symbol==\"rx_world_emergency_stop\") | .defined == true) and (.implementation[] | select(.symbol==\"operator_open\") | .defined == true) and (.tests[] | select(.name==\"R16_OP_SILICON\") | .target == \"test-r16-operator-silicon\") and (.tests[] | select(.name==\"RX_EMERGENCY\") | .status == \"PASS\" and .target == \"test-rx-emergency\")" "$R" >/dev/null'
g6log rx_emergency.log "RX_EMERGENCY_STOP: PASS (0 failures; host CPU, real caproot authority)"
out=$(G6Q)
check "G6 emergency test without the mutant pass line: test FAIL in the receipt, item FAIL, G6 never PASS" '! echo "$out" | grep -q "G6=PASS" && [ "$(g6_item operator_emergency_controls_passing)" = FAIL ] && jq -e ".g6_items[] | select(.item==\"operator_emergency_controls_passing\") | .tests[] | select(.name==\"RX_EMERGENCY\") | .status == \"FAIL\"" "$R" >/dev/null'
g6log rx_emergency.log "RX_EMERGENCY_MUTANTS: PASS"
out=$(G6Q)
check "G6 emergency test without the stop pass line: test FAIL in the receipt" 'jq -e ".g6_items[] | select(.item==\"operator_emergency_controls_passing\") | .tests[] | select(.name==\"RX_EMERGENCY\") | .status == \"FAIL\"" "$R" >/dev/null'
g6log rx_emergency.log "RX_EMERGENCY_STOP: PASS (0 failures; host CPU, real caproot authority)
RX_EMERGENCY_MUTANTS: PASS" 1
out=$(G6Q)
check "G6 emergency test exit 1 with both pass lines: test FAIL in the receipt" 'jq -e ".g6_items[] | select(.item==\"operator_emergency_controls_passing\") | .tests[] | select(.name==\"RX_EMERGENCY\") | .status == \"FAIL\"" "$R" >/dev/null'
g6log rx_emergency.log "RX_EMERGENCY_STOP: PASS (0 failures; host CPU, real caproot authority)
RX_EMERGENCY_MUTANTS: PASS"
out=$(G6Q)
rm -rf "$G"

# 9. the script holds no literal for observed fields and does not touch tracked evidence
check "no literal tree_dirty/candidate_bound/silicon_observed values" \
    '! grep -E "\"(tree_dirty|candidate_bound|silicon_observed)\": (true|false)" "$HERE/tools/r16_qualify.sh"'
check "no copy into tracked evidence/" '! grep -E "cp .*evidence/R16|EVID_RAW_DIR" "$HERE/tools/r16_qualify.sh"'
check "no hardcoded home paths" '! grep -E "/home/[a-z]" "$HERE/tools/r16_qualify.sh"'

if [ "$FAILS" -ne 0 ]; then echo "R16 qualify self-test: $FAILS FAILED"; exit 1; fi
echo "R16 qualify self-test: PASS"
