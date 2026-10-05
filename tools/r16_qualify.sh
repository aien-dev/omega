#!/bin/bash
# r16_qualify.sh -- R16 Orchestrator Retirement qualification harness (schema revision 2).
#
# Runs the R16 gate targets on the current checkout and writes the
# AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1 receipt (field names unchanged; the added field
# "schema_revision": 2 marks the changed semantics below).
#
# Truthfulness rules (revision 2):
#   * A dirty tree is REFUSED (exit 2) before anything runs.
#   * Every gate status is computed from a real check: a make target's exit status AND
#     a required line in its log (or a value read from the inventory JSON). A failing
#     check is FAIL. A gate with no implemented check is NOT_RUN, never PASS.
#   * candidate_bound, tree_dirty and silicon_observed are computed from what was
#     observed during this run, not written as literals.
#   * Raw evidence and the receipt go to an UNTRACKED directory (default build/r16-raw/<run>,
#     or R16_OUT_DIR). Nothing under evidence/ is touched; a human or agent step copies
#     and commits the receipt later.
#   * No hardcoded machine paths. PHYSICS_DIR / AIENOS_LOCK_REPO are passed to make
#     only if the caller set them.
#
# Exit status: 0 every gate PASS; 1 at least one FAIL; 2 refused (dirty tree / bad
# environment); 3 no FAIL but at least one gate NOT_RUN.
#
# Environment:
#   R16_OUT_DIR           output root (default <repo>/build/r16-raw); must not be tracked
#   R16_RUN_ID            override the run id (used by the self-test)
#   R16_EXPECT_COMMIT     if set, HEAD must equal it or the run is refused
#   R16_R15_RECEIPT       path to the R15 receipt on this candidate (checked for G7)
#   R16_QUALIFY_DRY=1     dry mode, used by tests/r16_qualify/run.sh: runs no make target, no
#                         chip, no machine probe; reads fixture logs (<log> and <log>.rc)
#                         already in the raw dir. Never writes a sha-named receipt.
set -euo pipefail

SCHEMA_REVISION=2
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$HERE"
DRY=${R16_QUALIFY_DRY:-0}

refuse() { echo "REFUSED: $*" >&2; exit 2; }

# ---- 0. refuse before running anything ------------------------------------
git rev-parse --git-dir >/dev/null 2>&1 || refuse "not a git work tree: $HERE"
if [ -n "$(git status --porcelain)" ]; then
    git status --porcelain | head -20 >&2
    refuse "working tree is dirty (uncommitted or untracked files); commit or clean first"
fi
CANDIDATE_COMMIT=$(git rev-parse HEAD)
if [ -n "${R16_EXPECT_COMMIT:-}" ] && [ "$R16_EXPECT_COMMIT" != "$CANDIDATE_COMMIT" ]; then
    refuse "HEAD $CANDIDATE_COMMIT is not the expected candidate $R16_EXPECT_COMMIT"
fi
export OMEGA_CANDIDATE_COMMIT="$CANDIDATE_COMMIT"
if [ -n "${PHYSICS_DIR:-}" ]; then export PHYSICS_DIR; fi
if [ -n "${AIENOS_LOCK_REPO:-}" ]; then export AIENOS_LOCK_REPO; fi

RUN_COMMIT=$CANDIDATE_COMMIT
RUN_ID=${R16_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)-${CANDIDATE_COMMIT:0:12}}
OUT_ROOT=${R16_OUT_DIR:-$HERE/build/r16-raw}
RAW_DIR="$OUT_ROOT/$RUN_ID"
case "$OUT_ROOT/" in
    "$HERE"/evidence/*) refuse "output dir is under tracked evidence/; use an untracked path" ;;
esac
mkdir -p "$RAW_DIR"
if [ "$DRY" != 1 ]; then
    case "$RAW_DIR" in
        "$HERE"/*) git check-ignore -q "$RAW_DIR" || refuse "output dir $RAW_DIR is inside the repo but not git-ignored" ;;
    esac
fi

echo "=== R16 Qualification (schema revision $SCHEMA_REVISION): $RUN_ID ==="
echo "Candidate commit: $CANDIDATE_COMMIT"
if [ "$DRY" = 1 ]; then echo "DRY MODE: no make target, no chip, no receipt named by hash"; fi

# ---- helpers -----------------------------------------------------------------
RC=0
# run_target <logfile> <make target...>: sets RC to the exit status, or NOT_RUN in dry
# mode when no fixture log exists. Never aborts the script (set -e is off around make).
run_target() {
    local log=$1; shift
    if [ "$DRY" = 1 ]; then
        if [ -f "$log" ]; then RC=$(cat "$log.rc" 2>/dev/null || echo 0); else RC=NOT_RUN; fi
        return 0
    fi
    set +e
    make "$@" > "$log" 2>&1 < /dev/null
    RC=$?
    set -e
    echo "$RC" > "$log.rc"
}
# verdict <rc> <logfile> <fixed pattern>: PASS only if the target exited 0 and the log
# holds the pattern; FAIL if it ran and either failed; NOT_RUN if it did not run.
verdict() {
    local rc=$1 log=$2 pat=$3
    if [ "$rc" = NOT_RUN ]; then echo NOT_RUN; return; fi
    if [ "$rc" != 0 ]; then echo FAIL; return; fi
    if [ -s "$log" ] && grep -Fq -- "$pat" "$log"; then echo PASS; else echo FAIL; fi
}
# combine <status...>: FAIL beats NOT_RUN (or MISSING_IMPLEMENTATION) beats PASS.
combine() {
    local s r=PASS
    for s in "$@"; do
        if [ "$s" = FAIL ]; then echo FAIL; return; fi
        if [ "$s" != PASS ]; then r=NOT_RUN; fi   # NOT_RUN, MISSING_IMPLEMENTATION, anything else
    done
    echo "$r"
}
# jint <json> <key>: integer value of a key (last occurrence), empty if absent.
jint() {
    [ -s "$1" ] || return 0
    sed -n 's/^.*"'"$2"'": *\([0-9][0-9]*\).*$/\1/p' "$1" | tail -1
}
jstrv() {
    [ -s "$1" ] || return 0
    sed -n 's/^.*"'"$2"'": *"\([^"]*\)".*$/\1/p' "$1" | tail -1
}
repohead() {
    [ -s "$1" ] || return 0
    sed -n 's/^.*"name": "'"$2"'".*"head": "\([0-9a-f]*\)".*$/\1/p' "$1" | head -1
}
b2j() { if [ "$1" = PASS ]; then echo true; elif [ "$1" = FAIL ]; then echo false; else echo '"NOT_RUN"'; fi; }

# ---- 1. machine identity (observation only; no chip access) -----------------
AIENOS_COMMIT=$(cat "$HERE/aienos.lock" 2>/dev/null || echo "unknown")
PHYSICS_COMMIT=$(cat "$HERE/physics.lock" 2>/dev/null || echo "unknown")
if [ "$DRY" != 1 ]; then
    echo "[*] Capturing machine identity..."
    midrs=$(for c in /sys/devices/system/cpu/cpu[0-9]*; do
        printf '%s:%s ' "${c##*cpu}" "$(cat "$c/regs/identification/midr_el1" 2>/dev/null || true)"; done) || true
    govs=$(for c in /sys/devices/system/cpu/cpu[0-9]*; do
        printf '%s:%s:%s ' "${c##*cpu}" "$(cat "$c/cpufreq/scaling_governor" 2>/dev/null || true)" \
            "$(cat "$c/cpufreq/scaling_cur_freq" 2>/dev/null || true)"; done) || true
    top=$(ps -eo pcpu,comm --sort=-pcpu 2>/dev/null | sed -n '2,11p' | awk '{printf "%s:%s ", $2, $1}') || true
    gpu_info=$(nvidia-smi --query-gpu=name,pci.bus_id,driver_version,temperature.gpu --format=csv,noheader 2>/dev/null) || gpu_info="N/A"
    mach_id=$(sha256sum /etc/machine-id 2>/dev/null | cut -d' ' -f1) || mach_id="N/A"
    mem_total=$(awk '/MemTotal/{print $2}' /proc/meminfo 2>/dev/null) || mem_total=0
    cat > "$RAW_DIR/machine.json" <<MEOF
{
  "hostname": "$(hostname)",
  "machine_id_sha256": "$mach_id",
  "kernel": "$(uname -r)",
  "architecture": "$(uname -m)",
  "midr": "$midrs",
  "governor_freq": "$govs",
  "mem_kb": "$mem_total",
  "gpu": "$gpu_info",
  "loadavg": "$(cat /proc/loadavg 2>/dev/null || true)",
  "top_cpu": "$top",
  "aienos_commit": "$AIENOS_COMMIT",
  "physics_commit": "$PHYSICS_COMMIT",
  "compiler": "$(${CC:-cc} --version 2>/dev/null | head -1 || true)"
}
MEOF
fi

# ---- 2. G1 / G2: loop inventory -------------------------------------------
echo "[*] R16-G1 / R16-G2: loop inventory..."
INV_LOG="$RAW_DIR/r16_inventory.log"
INV_JSON="$RAW_DIR/inventory.json"
INV_TEST_LOG="$RAW_DIR/test_r16_inventory.log"
if [ "$DRY" != 1 ]; then rm -f build/r16-inventory.json; fi
run_target "$INV_LOG" r16-inventory
INV_RC=$RC
if [ "$DRY" != 1 ] && [ -s build/r16-inventory.json ]; then cp build/r16-inventory.json "$INV_JSON"; fi
run_target "$INV_TEST_LOG" test-r16-inventory
INV_TEST_RC=$RC
G1=NOT_RUN; G2=NOT_RUN; UNCLASS=""; AREACH=""
if [ "$INV_RC" != NOT_RUN ] || [ -s "$INV_JSON" ]; then
    if [ ! -s "$INV_JSON" ]; then
        G1=FAIL; G2=FAIL
    else
        UNCLASS=$(jint "$INV_JSON" unclassified)
        AREACH=$(jint "$INV_JSON" a_reachable)
        g1=PASS
        for k in unclassified question_rows bad_class stale_rows map_errors skipped_repos; do
            [ "$(jint "$INV_JSON" "$k")" = 0 ] || g1=FAIL
        done
        for r in omega aien-sovereign-core aegis-runtime aienos physics; do
            [ -n "$(repohead "$INV_JSON" "$r")" ] || g1=FAIL   # every scanned commit recorded
        done
        G1=$g1
        g2=PASS
        [ "$INV_RC" = 0 ] || g2=FAIL
        [ "$(jstrv "$INV_JSON" result)" = PASS ] || g2=FAIL
        [ "$AREACH" = 0 ] || g2=FAIL
        G2=$(combine "$g2" "$(verdict "$INV_TEST_RC" "$INV_TEST_LOG" "R16 inventory self-test: PASS")")
    fi
fi
echo "    G1=$G1 G2=$G2 (unclassified=${UNCLASS:-n/a} a_reachable=${AREACH:-n/a})"

# ---- 3. G3: authoritative path without legacy orchestrators ------------------
echo "[*] R16-G3: authpath host and silicon..."
SILICON_BROKEN=0
V=NOT_RUN
run_target "$RAW_DIR/r16_authpath_host.log" test-r16-authpath
G3H=$(verdict "$RC" "$RAW_DIR/r16_authpath_host.log" "R16 gate: R16_G3_AUTHPATH=HOST_PASS_NON_SILICON")
# silicon_run <name> <log> <pattern> <make target>: sets V. After one silicon failure the
# remaining silicon targets are NOT run (a failed seat run can leave the chip in a bad state).
silicon_run() {
    local log=$2 pat=$3; shift 3
    if [ "$SILICON_BROKEN" = 1 ]; then V=NOT_RUN; return 0; fi
    run_target "$log" "$@"
    V=$(verdict "$RC" "$log" "$pat")
    if [ "$V" = FAIL ]; then SILICON_BROKEN=1; fi
    return 0
}
silicon_run g3s "$RAW_DIR/r16_authpath_silicon.log" "R16 gate: R16_G3_AUTHPATH=PASS" test-r16-authpath-silicon
G3S=$V
G3=$(combine "$G3H" "$G3S")
echo "    G3=$G3 (host $G3H, silicon $G3S)"

# ---- 4. G4: negative tests and load-bearing mutants -------------------------
echo "[*] R16-G4: negative tests and mutants..."
run_target "$RAW_DIR/r16_negative.log" test-r16-negative
G4A=$(verdict "$RC" "$RAW_DIR/r16_negative.log" "R16 gate: R16_G4_LEGACY_REFUSED=PASS")
run_target "$RAW_DIR/r16_negative_mutants.log" test-r16-negative-mutants
G4B=$(verdict "$RC" "$RAW_DIR/r16_negative_mutants.log" "R16 gate: R16_G4_GUARDS_LOAD_BEARING=PASS")
G4=$(combine "$G4A" "$G4B")
echo "    G4=$G4"

# ---- 5. G5: surface ----------------------------------------------------------
echo "[*] R16-G5: surface check..."
run_target "$RAW_DIR/r16_surface.log" test-r16-surface
G5=$(verdict "$RC" "$RAW_DIR/r16_surface.log" "R16 gate: R16_G5_SURFACE=PASS")
echo "    G5=$G5"

# ---- 6. G7: R1-R15 ladder on the candidate ----------------------------------
echo "[*] R16-G7: ladder..."
# name|make target|log|pattern|silicon(0/1)
LADDER='R1_R6|test-r3|r1_r6_heartbeat.log|R4_CAUSAL_TRACE: PASS|0
R7|test-r7|r7_native.log|native authority matched the linux oracle|0
R8|test-r8|r8_aegis.log|failures 0|0
R9|test-r9|r9_barrier.log|generation barrier kept a single coherent generation|0
R10|test-r10|r10_omega.log|failures 0|0
R11|test-r11|r11_aien.log|failures 0|0
R12_host|test-r12|r12_host.log|failures 0|0
R12_silicon|test-r12-silicon|r12_silicon.log|silicon 1|1
R13_host|test-r13-host|r13_host.log|R13 gate: R13_LIVING_SYSTEM=HOST_PASS_NON_SILICON|0
R13_silicon|test-r13-silicon|r13_silicon.log|R13 gate: R13_LIVING_SYSTEM=PASS|1
R14_host|test-r14-host|r14_host.log|R14 gate: R14_LIVING_RECOVERY=HOST_PASS_NON_SILICON|0
R14_silicon|test-r14-silicon|r14_silicon.log|R14 gate: R14_LIVING_RECOVERY=PASS|1
R15_parity_host|test-r15-parity-host|r15_parity_host.log|SEQ_SEMANTIC_PARITY=PASS|0
R15_g7_host|test-r15-g7-host|r15_g7_host.log|R15 G7 worker split: 57 checks, 0 failures|0
R15_parity_silicon|test-r15-parity-silicon|r15_parity_silicon.log|SEQ_SEMANTIC_PARITY=PASS|1
R15_receipt|test-r15-receipt|r15_receipt.log|r15 receipt test: 0 failure(s)|0'
declare -A LV TG LOGF
SILICON_RAN=0; SILICON_PASS=0
if [ "$G3S" != NOT_RUN ]; then
    SILICON_RAN=1
    if [ "$G3S" = PASS ]; then SILICON_PASS=1; fi
fi
while IFS='|' read -r name target log pat sil; do
    TG[$name]=$target; LOGF[$name]="$RAW_DIR/$log"
    if [ "$sil" = 1 ]; then
        silicon_run "$name" "$RAW_DIR/$log" "$pat" "$target"
        LV[$name]=$V
        if [ "$V" != NOT_RUN ]; then SILICON_RAN=$((SILICON_RAN + 1)); fi
        if [ "$V" = PASS ]; then SILICON_PASS=$((SILICON_PASS + 1)); fi
    else
        run_target "$RAW_DIR/$log" "$target"
        LV[$name]=$(verdict "$RC" "$RAW_DIR/$log" "$pat")
        # A rung whose living part did not run (R11 refuses to start above load 2 and still
        # exits 0 with "failures 0") is NOT_RUN, never PASS.
        if [ "${LV[$name]}" = PASS ] && grep -Fq -- "[-] living run not exercised" "$RAW_DIR/$log"; then
            LV[$name]=NOT_RUN
        fi
    fi
    echo "    $name: ${LV[$name]}"
done <<< "$LADDER"

# R15 physical acceptance (R15 spec G1-G16) is judged from the R15 receipt on this
# candidate, supplied by the caller. No receipt: NOT_RUN.
R15ACC=NOT_RUN
if [ -n "${R16_R15_RECEIPT:-}" ]; then
    R15ACC=PASS
    f=$R16_R15_RECEIPT
    if [ ! -s "$f" ]; then
        R15ACC=FAIL
    else
        [ "$(basename "$f" .json)" = "$(sha256sum "$f" | cut -d' ' -f1)" ] || R15ACC=FAIL
        grep -Fq "\"outcome\": \"PASS\"" "$f" || R15ACC=FAIL
        grep -Fq "\"candidate_commit\": \"$CANDIDATE_COMMIT\"" "$f" || R15ACC=FAIL
        grep -Fq "\"candidate_bound\": true" "$f" || R15ACC=FAIL
        grep -Fq "\"tree_dirty\": false" "$f" || R15ACC=FAIL
        grep -Fq "\"silicon_observed\": true" "$f" || R15ACC=FAIL
    fi
fi
G7=$(combine "${LV[R1_R6]}" "${LV[R7]}" "${LV[R8]}" "${LV[R9]}" "${LV[R10]}" "${LV[R11]}" "${LV[R12_host]}" \
    "${LV[R12_silicon]}" "${LV[R13_host]}" "${LV[R13_silicon]}" "${LV[R14_host]}" "${LV[R14_silicon]}" \
    "${LV[R15_parity_host]}" "${LV[R15_g7_host]}" "${LV[R15_parity_silicon]}" "${LV[R15_receipt]}" "$R15ACC")
echo "    G7=$G7 (R15 acceptance receipt: $R15ACC)"

# ---- 7. G6: protected things kept -------------------------------------------
# The twelve items of spec R16-G6: the six section-49 items plus R9 crash recovery, R10
# verifier, R12 seat-loss handling, R14 recovery paths, operator emergency controls and
# the benchmark reference paths (SEQ). Presence and operation are separate facts:
#   * present  = every named implementation symbol is DEFINED in its file (a function
#     definition line; a comment, prototype or call does not count). Absent: MISSING_IMPLEMENTATION.
#   * operated = every named exercising test ran in THIS invocation and passed (exit
#     status AND required log line). A log that reports a skipped or unexercised run is
#     NOT_RUN, never PASS. FAIL if any test ran and failed.
# Item status: MISSING_IMPLEMENTATION if not present; else FAIL > NOT_RUN > PASS over its
# tests. G6 combines the items like combine() (FAIL > NOT_RUN/MISSING > PASS); G6 names
# MISSING_IMPLEMENTATION when an item is missing and nothing failed.
echo "[*] R16-G6: protected things kept..."
# g6_defined <file> <symbol> <f|t>: f = C function definition line, t = fixed text.
g6_defined() {
    [ -f "$HERE/$1" ] || return 1
    if [ "$3" = t ]; then grep -Fq -- "$2" "$HERE/$1"
    else grep -Eq -- "^[A-Za-z_][^;]*[ *]$2\\([^;]*\$" "$HERE/$1"; fi
}
# M19 accelerator world (the known-good fallback) and its maintenance entry: one silicon run.
silicon_run m19 "$RAW_DIR/m19_world.log" "M19 GATES PASSED" test-m19
LV[M19]=$V; TG[M19]=test-m19; LOGF[M19]="$RAW_DIR/m19_world.log"
if [ "${LV[M19]}" = PASS ] && grep -Fq -- "[FAIL]" "$RAW_DIR/m19_world.log"; then LV[M19]=FAIL; fi
# g6_test <name>: status of one exercising test; a skipped or unexercised log is NOT_RUN.
g6_test() {
    local s=${LV[$1]:-NOT_RUN}
    if [ "$s" = PASS ] && [ -s "${LOGF[$1]:-/nonexistent}" ] && grep -Eq -- "SKIPPED|not exercised" "${LOGF[$1]}"; then s=NOT_RUN; fi
    echo "$s"
}
# key|file:symbol:kind;...|tests (comma separated)
G6_ITEMS='known_good_fallback_present|src/omega_accelerator_world.c:omega_world_drain:f;src/omega_accelerator_world.c:omega_world_init:f|M19
recovery_path_present|src/runtime/rx_generation.c:rx_gen_recover:f|R9,R14_host
deterministic_maintenance_controls_present|src/omega_world_gates.c:run_m19_gates:f;tools/omegatool.c:--run-m19-gates:t|M19
trusted_capability_root_present|src/runtime/rx_native_bind.c:rx_world_init_native:f;src/runtime/rx_native_bind.c:native_validate:f|R7,R12_host
generation_mechanism_present|src/runtime/rx_generation.c:rx_gen_propose:f|R9,R13_host
evidence_present|src/omega_evidence.c:omega_evidence_write_digest:f|R15_receipt,R9
r9_crash_recovery_passing|src/runtime/rx_generation.c:rx_gen_open:f|R9
r10_verifier_passing|src/omega_verify.c:omega_verify_v0_structural:f|R10
r12_seat_loss_handling_passing|src/runtime/rx_resident_gpu.c:rx_gpu_seat_kill:f|R12_host,R12_silicon
r14_recovery_paths_passing|src/runtime/rx_generation.c:rx_gen_recover:f|R14_host,R14_silicon
operator_emergency_controls_passing|src/runtime/rx_resident_gpu.c:rx_gpu_seat_kill:f|R12_silicon
benchmark_reference_paths_seq_passing|src/runtime/rx_seq_reference.c:rx_seq_pulse:f|R15_parity_host,R15_parity_silicon'
declare -A G6S
G6LIST=""; G6ALL=""; G6MISSING=0
while IFS='|' read -r key pairs tests; do
    present=true; implj=""
    IFS=';' read -ra PA <<< "$pairs"
    for p in "${PA[@]}"; do
        pf=${p%%:*}; rest=${p#*:}; kind=${rest##*:}; sym=${rest%:*}
        if g6_defined "$pf" "$sym" "$kind"; then d=true; else d=false; present=false; fi
        implj="$implj{\"file\": \"$pf\", \"symbol\": \"$sym\", \"defined\": $d}, "
    done
    implj=${implj%, }
    tj=""; ts=PASS; IFS=',' read -ra TA <<< "$tests"
    for t in "${TA[@]}"; do
        s=$(g6_test "$t"); ts=$(combine "$ts" "$s")
        tj="$tj{\"name\": \"$t\", \"target\": \"${TG[$t]}\", \"status\": \"$s\"}, "
    done
    tj=${tj%, }
    if [ "$present" = false ]; then st=MISSING_IMPLEMENTATION; G6MISSING=$((G6MISSING + 1)); else st=$ts; fi
    G6S[$key]=$st; G6ALL="$G6ALL $st"
    G6LIST="$G6LIST    {\"item\": \"$key\", \"status\": \"$st\", \"present\": $present, \"implementation\": [$implj], \"tests\": [$tj], \"basis\": \"present = symbol definition found; status PASS only if every test ran and passed in this run\"},
"
    echo "    $key: $st"
done <<< "$G6_ITEMS"
G6LIST=${G6LIST%$'\n'}; G6LIST=${G6LIST%,}
# shellcheck disable=SC2086
G6=$(combine $G6ALL)
if [ "$G6" = NOT_RUN ] && [ "$G6MISSING" -gt 0 ] && [[ " $G6ALL " != *" FAIL "* ]]; then G6=MISSING_IMPLEMENTATION; fi
echo "    G6=$G6 (items missing: $G6MISSING)"
g6j() { local s=${G6S[$1]}; if [ "$s" = PASS ]; then echo true; elif [ "$s" = FAIL ]; then echo false; else echo "\"$s\""; fi; }

# ---- 8. observed binding ----------------------------------------------------------
END_COMMIT=$(git rev-parse HEAD)
if [ -n "$(git status --porcelain)" ]; then TREE_DIRTY=true; else TREE_DIRTY=false; fi
if [ "$END_COMMIT" = "$CANDIDATE_COMMIT" ] && [ "$RUN_COMMIT" = "$CANDIDATE_COMMIT" ] && [ "$TREE_DIRTY" = false ]; then
    CAND_BOUND=true
else
    CAND_BOUND=false
fi
# silicon_observed: every silicon target (authpath, R12, R13, R14, R15 parity) ran and passed.
if [ "$SILICON_RAN" -ge 5 ] && [ "$SILICON_PASS" = "$SILICON_RAN" ]; then SIL_OBS=true; else SIL_OBS=false; fi

# ---- 9. receipt ---------------------------------------------------------------------
SC_COMMIT=$(repohead "$INV_JSON" aien-sovereign-core)
AR_COMMIT=$(repohead "$INV_JSON" aegis-runtime)
REMAINING_CENTRAL='"NOT_RUN"'
if [ -n "$AREACH" ]; then REMAINING_CENTRAL=$AREACH; fi
REMAINING_UNCLASS='"NOT_RUN"'
if [ -n "$UNCLASS" ]; then REMAINING_UNCLASS=$UNCLASS; fi

# G8 (spec/r16-orchestrator-retirement.md §G8) includes "the PR is merged with a merge
# commit" and silicon_observed = true. This script runs before any merge, so it can
# never observe the whole gate: G8 is always NOT_RUN here. It reports only whether the
# receipt preconditions hold (bound, clean tree, silicon observed, not dry); G8 is
# decided after the merge by whoever checks the merge commit.
if [ "$CAND_BOUND" = true ] && [ "$TREE_DIRTY" = false ] && [ "$SIL_OBS" = true ] && [ "$DRY" != 1 ]; then
    G8_PRECONDITIONS=met
else
    G8_PRECONDITIONS=not_met
fi
G8=NOT_RUN
echo "[*] R16-G8: NOT_RUN (merge-commit part is outside this script; receipt preconditions $G8_PRECONDITIONS)"

OVERALL=$(combine "$G1" "$G2" "$G3" "$G4" "$G5" "$G6" "$G7" "$G8")

lr() { printf '    "%s": {"status": "%s", "target": "%s"}' "$1" "${LV[$2]}" "${TG[$2]}"; }
RAW_DIGEST="NOT_RUN"
(cd "$RAW_DIR" && rm -f SHA256SUMS && ls | grep -v '^SHA256SUMS$' | LC_ALL=C sort | xargs sha256sum > SHA256SUMS) || true
if [ -s "$RAW_DIR/SHA256SUMS" ]; then RAW_DIGEST=$(sha256sum "$RAW_DIR/SHA256SUMS" | cut -d' ' -f1); fi
RAW_REL=${RAW_DIR#"$HERE"/}

OUT_RECEIPT_TMP="$RAW_DIR/receipt.raw.json"
cat > "$OUT_RECEIPT_TMP" <<RECOBJ
{
  "schema": "AIEN_RX_R16_ORCHESTRATOR_RETIRED_V1",
  "schema_revision": $SCHEMA_REVISION,
  "dry_run": $(if [ "$DRY" = 1 ]; then echo true; else echo false; fi),
  "candidate_commit": "$CANDIDATE_COMMIT",
  "run_commit": "$RUN_COMMIT",
  "candidate_bound": $CAND_BOUND,
  "tree_dirty": $TREE_DIRTY,
  "silicon_observed": $SIL_OBS,
  "aienos_commit": "$AIENOS_COMMIT",
  "physics_commit": "$PHYSICS_COMMIT",
  "aienos_lock": "$AIENOS_COMMIT",
  "physics_lock": "$PHYSICS_COMMIT",
  "aien_sovereign_core_commit": "${SC_COMMIT:-unknown}",
  "aegis_runtime_commit": "${AR_COMMIT:-unknown}",
  "production_entry_point": "docs/r16-production-entry-point.md",
  "legacy_orchestrators_disabled_test": "$G4A",
  "legacy_cannot_bypass_authority_test": "$G3",
  "remaining_central_loop_count": $REMAINING_CENTRAL,
  "remaining_unclassified_semantic_loop_count": $REMAINING_UNCLASS,
  "protected_surfaces_kept": {
    "known_good_fallback_present": $(g6j known_good_fallback_present),
    "recovery_path_present": $(g6j recovery_path_present),
    "deterministic_maintenance_controls_present": $(g6j deterministic_maintenance_controls_present),
    "trusted_capability_root_present": $(g6j trusted_capability_root_present),
    "generation_mechanism_present": $(g6j generation_mechanism_present),
    "evidence_present": $(g6j evidence_present),
    "r9_crash_recovery_passing": $(g6j r9_crash_recovery_passing),
    "r10_verifier_passing": $(g6j r10_verifier_passing),
    "r12_seat_loss_handling_passing": $(g6j r12_seat_loss_handling_passing),
    "r14_recovery_paths_passing": $(g6j r14_recovery_paths_passing),
    "operator_emergency_controls_passing": $(g6j operator_emergency_controls_passing),
    "benchmark_reference_paths_seq_passing": $(g6j benchmark_reference_paths_seq_passing)
  },
  "g6_items": [
$G6LIST
  ],
  "r15_acceptance_still_passing": $(b2j "$R15ACC"),
  "correctness_reruns": {
$(lr R1 R1_R6),
$(lr R2 R1_R6),
$(lr R3 R1_R6),
$(lr R4 R1_R6),
$(lr R5 R1_R6),
$(lr R6 R1_R6),
$(lr R7 R7),
$(lr R8 R8),
$(lr R9 R9),
$(lr R10 R10),
$(lr R11 R11),
$(lr R12_host R12_host),
$(lr R12_silicon R12_silicon),
$(lr R13_host R13_host),
$(lr R13_silicon R13_silicon),
$(lr R14_host R14_host),
$(lr R14_silicon R14_silicon),
$(lr R15_parity_host R15_parity_host),
$(lr R15_parity_silicon R15_parity_silicon),
$(lr R15_g7_host R15_g7_host),
$(lr R15_receipt R15_receipt)
  },
  "gates": {
    "R16-G1": "$G1",
    "R16-G2": "$G2",
    "R16-G3": "$G3",
    "R16-G4": "$G4",
    "R16-G5": "$G5",
    "R16-G6": "$G6",
    "R16-G7": "$G7",
    "R16-G8": "$G8"
  },
  "g8_scope": "receipt bound to the candidate, clean tree, named by its own SHA-256; the merge commit is a separate step not checked here",
  "gate": "$OVERALL",
  "R16_ORCHESTRATOR_RETIRED": "$OVERALL",
  "scope": "ADR 0016 resident reaction architecture migration; sovereign-core LLM request loop: not retired, still in use",
  "not_claimed": [
    "Retirement of the aien-sovereign-core LLM request loop (section 3.1, Q1)",
    "Removal of any Rust code (section 3.1, Q2); the Rust loops still run if started by hand outside the AIEN production path",
    "Retirement or code removal of the aien-sovereign-core aien-cli operator tool loops (13 rows) and spark-dream idle-time cycle loop (1 row); classified as class A (retired by non-use), their code is not removed under section 3.1 Q2",
    "Any gate or field written NOT_RUN or MISSING_IMPLEMENTATION: no check or implementation was found for it, or its target was not run in this run"
  ],
  "raw_digest_note": "sha256 of SHA256SUMS over the raw directory",
  "raw_directory_digest_sha256": "$RAW_DIGEST",
  "raw_directory": "$RAW_REL",
  "timestamp_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
RECOBJ

if [ "$DRY" = 1 ]; then
    mv "$OUT_RECEIPT_TMP" "$RAW_DIR/DRY-RUN-receipt.json"
    FINAL_PATH="$RAW_DIR/DRY-RUN-receipt.json"
else
    # Canonical writer built into the untracked output dir. The receipt goes beside the
    # raw dir, untracked; a later human/agent step copies it under evidence/R16/ and commits.
    ${CC:-cc} -std=gnu11 -O2 -Isrc -o "$RAW_DIR/json_canon" tools/json_canon.c src/sha256.c -lm
    "$RAW_DIR/json_canon" --pretty < "$OUT_RECEIPT_TMP" > "$RAW_DIR/receipt.pretty.json"
    RECEIPT_SHA=$(sha256sum "$RAW_DIR/receipt.pretty.json" | cut -d' ' -f1)
    mkdir -p "$OUT_ROOT/receipts"
    FINAL_PATH="$OUT_ROOT/receipts/$RECEIPT_SHA.json"
    "$RAW_DIR/json_canon" --write-exclusive "$FINAL_PATH" < "$RAW_DIR/receipt.pretty.json"
    chmod 0444 "$FINAL_PATH"
    if [ "$(sha256sum "$FINAL_PATH" | cut -d' ' -f1)" != "$RECEIPT_SHA" ]; then
        echo "ERROR: receipt not named by its own SHA-256" >&2
        exit 1
    fi
    rm -f "$OUT_RECEIPT_TMP" "$RAW_DIR/receipt.pretty.json"
fi

echo "Gates: G1=$G1 G2=$G2 G3=$G3 G4=$G4 G5=$G5 G6=$G6 G7=$G7 G8=$G8"
echo "Receipt (untracked): $FINAL_PATH"
echo "R16_QUALIFY_RESULT=$OVERALL"
case "$OVERALL" in
    PASS) exit 0 ;;
    FAIL) exit 1 ;;
    *) exit 3 ;;
esac
