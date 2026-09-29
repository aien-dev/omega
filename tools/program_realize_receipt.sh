#!/bin/bash
# program_realize_receipt.sh -- qualification receipt for program realization V0
# (spec/program-realization.md): realization is driven by the actual Omega program.
#
#   tools/program_realize_receipt.sh [out-dir]      (default: evidence/REALIZE)
#
# Runs, on the committed tree only (AArch64 host: the realization test executes code natively):
#   - make test-program-realize                      (4 gates, physics-free)
#   - regression: test-program-id, test-effect-cap64, test-visor-core, test-visor-world,
#     test-visor-authority, the Visor V1 qualification campaign (run_qualification.sh),
#     omegatool test (M4), test-m8 test-m9 test-m10 test-m11 test-m14, test-crumbline, and the
#     non-energy host suites r3 r7 r8 r9 r10 r11 r12 action-graph state-projection
#     capability-query typed-results
# Every make is wrapped in `flock $BENCH_LOCK`. Nothing under evidence/ is rewritten: the
# receipt is a NEW file named by the SHA-256 of its own bytes. It says FAIL (and why) when any
# suite fails, the tree is dirty or changes during the run, or a pin mismatches.
# Shell and coreutils only (git, sha256sum, grep, sed, awk, tar, make). No Python.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 2
OUTDIR=${1:-evidence/REALIZE}
AIENOS_REPO=${AIENOS_REPO:-$HOME/workspace/aienos-repo}
PHYSICS_DIR=${PHYSICS_DIR:-$HOME/workspace/physics-r13}
BENCH_LOCK=${BENCH_LOCK:-$HOME/workspace/.argus-bench.lock}
Q=build/przq
LOGS=$Q/logs
REASONS=()

head0=$(git rev-parse HEAD)
tree0=$(git rev-parse 'HEAD^{tree}')
dirty0=$(git status --porcelain)
[ -z "$dirty0" ] || REASONS+=("tree dirty at start")
run_id=$(date -u +%Y%m%dT%H%M%SZ)
[ "$(uname -m)" = aarch64 ] || REASONS+=("host is $(uname -m), not aarch64: native differential cannot run")

rm -rf "$Q" && mkdir -p "$LOGS" "$Q/aienos"
# ---- pins -------------------------------------------------------------------
aienos_pin=$(awk 'NF && $1 !~ /^#/ {print $1; exit}' aienos.lock)
physics_pin=$(awk 'NF && $1 !~ /^#/ {print $1; exit}' physics.lock)
physics_head=$(git -C "$PHYSICS_DIR" rev-parse HEAD 2>/dev/null || echo missing)
[ "$physics_head" = "$physics_pin" ] || REASONS+=("physics checkout $physics_head != physics.lock $physics_pin")

aienos_src_tree=$(git -C "$AIENOS_REPO" rev-parse "$aienos_pin:native/capability" 2>/dev/null || echo missing)
if git -C "$AIENOS_REPO" archive "$aienos_pin" native/capability | tar -x -C "$Q/aienos" &&
   flock "$BENCH_LOCK" make -C "$Q/aienos/native/capability" > "$LOGS/aienos-lib.log" 2>&1; then
    lib_ok=1
else
    lib_ok=0
    REASONS+=("AIENOS capability library did not build from $aienos_pin")
fi
LIB=$Q/aienos/native/capability/out/libaienos_capability.a
lib_sha=$( [ -f "$LIB" ] && sha256sum "$LIB" | awk '{print $1}' || echo null)

# ---- suites -----------------------------------------------------------------
SUITES_JSON=""
declare -A RC RUN FAILED

counts() {   # -> "run failed source"
    local log=$1 r
    r=$(awk '/^(PASS|FAIL) [0-9]+\/[0-9]+ *$/ { split($2, x, "/"); r += x[2]; f += x[2] - x[1]; n++ }
             /TOTAL GATES: *[0-9]+ *\| *PASSED: *[0-9]+ *\| *FAILED: *[0-9]+/ {
                 match($0, /TOTAL GATES: *[0-9]+/); t = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", t)
                 match($0, /FAILED: *[0-9]+/); u = substr($0, RSTART, RLENGTH); gsub(/[^0-9]/, "", u)
                 r += t; f += u; n++ }
             /^CRUMBLINE_CONFORMANCE pass=[0-9]+ fail=[0-9]+/ {
                 split($2, p, "="); split($3, q, "="); r += p[2] + q[2]; f += q[2]; n++ }
             /^QUAL_SECTION [a-z]+ run=[0-9]+ failed=[0-9]+/ {
                 split($3, p, "="); split($4, q, "="); r += p[2]; f += q[2]; n++ }
             END { if (n) printf "%d %d\n", r, f }' "$log")
    if [ -n "$r" ]; then echo "$r output"; return; fi
    local rcpt
    rcpt=$(grep -oE 'receipt: [^ ]+\.json' "$log" | tail -n 1 | awk '{print $2}')
    if [ -n "$rcpt" ] && [ -f "$rcpt" ]; then
        local line c f
        line=$(grep -m1 '^  "totals":' "$rcpt")
        if [ -n "$line" ]; then
            c=$(printf '%s' "$line" | grep -oE '"checks": [0-9]+' | grep -oE '[0-9]+')
            f=$(printf '%s' "$line" | grep -oE '"failures": [0-9]+' | grep -oE '[0-9]+')
        else
            c=$(grep -m1 -E '^  "checks": [0-9]+' "$rcpt" | grep -oE '[0-9]+')
            f=$(grep -m1 -E '^  "failures": [0-9]+' "$rcpt" | grep -oE '[0-9]+')
        fi
        if [ -n "$c" ] && [ -n "$f" ]; then echo "$c $f receipt:$rcpt"; return; fi
    fi
    echo "0 0 exit-status-only"
}

record() {   # record <name> <rc> <log>
    local name=$1 rc=$2 log=$3 n f src
    read -r n f src < <(counts "$log")
    RC[$name]=$rc
    RUN[$name]=$n
    FAILED[$name]=$f
    local status=FAIL
    [ "$rc" -eq 0 ] && [ "$f" -eq 0 ] && status=PASS
    [ "$status" = PASS ] || REASONS+=("$name $status (rc=$rc, $f of $n failed)")
    SUITES_JSON+="${SUITES_JSON:+,}
    {\"target\": \"$name\", \"rc\": $rc, \"tests_run\": $n, \"tests_failed\": $f, \"count_source\": \"$src\", \"status\": \"$status\", \"log_sha256\": \"$(sha256sum "$log" | awk '{print $1}')\"}"
    echo "suite $name: $status (rc=$rc run=$n failed=$f)" >&2
}

suite() {    # suite <name> <out-dir> <make args...>
    local name=$1 out=$2
    shift 2
    local log=$LOGS/$name.log
    flock "$BENCH_LOCK" env OMEGA_CANDIDATE_COMMIT="$head0" AIENOS_COMMIT="$aienos_pin" \
        PHYSICS_COMMIT="$physics_pin" make OUT_DIR="$out" "$@" "$name" > "$log" 2>&1
    record "$name" $? "$log"
}

PF=(PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 AIENOS_R7_DIR="$Q/aienos")
RT=(PHYSICS_DIR="$PHYSICS_DIR" AIENOS_R7_DIR="$Q/aienos" VISOR_AUTH_CAP_LIB="$LIB")

suite test-program-realize "$Q/pf" "${PF[@]}"
suite test-program-id "$Q/pf" "${PF[@]}"
suite test-visor-core "$Q/pf" "${PF[@]}"
suite test-visor-world "$Q/pf" "${RT[@]}"
suite test-visor-authority "$Q/pf" "${RT[@]}"

# Visor V1 qualification campaign (shell), physics-free omega binary.
qlog=$LOGS/visor-qualification.log
{ flock "$BENCH_LOCK" make OUT_DIR="$Q/q8" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 "$Q/q8/omega" &&
  flock "$BENCH_LOCK" env QUAL_WORK="$Q/q8/qual-work" tests/visor/qualification/run_qualification.sh "$Q/q8/omega"; } \
    > "$qlog" 2>&1
record visor-qualification $? "$qlog"

CONSUMERS=(test-m8 test-m9 test-m10 test-m11 test-m14 test-crumbline)
for t in "${CONSUMERS[@]}"; do suite "$t" "$Q/reg" "${RT[@]}"; done

HOST=(test-effect-cap64 test test-r3 test-r7 test-r8 test-r9 test-r10 test-r11 test-r12 test-action-graph
      test-state-projection test-capability-query test-typed-results)
suite test-effect-cap64 "$Q/pf" "${PF[@]}"
for t in "${HOST[@]:1}"; do suite "$t" "$Q/reg" "${RT[@]}"; done

REGRESSION=(test-program-id test-visor-core test-visor-world test-visor-authority visor-qualification
            "${CONSUMERS[@]}" "${HOST[@]}")

# ---- gates ------------------------------------------------------------------
prz_log=$LOGS/test-program-realize.log
gate_line() {   # -> "STATUS passed total"
    local line
    line=$(grep -E "^GATE $1 (PASS|FAIL) [0-9]+/[0-9]+$" "$prz_log" | tail -n 1)
    [ -n "$line" ] || { echo "NOT_RUN 0 0"; return; }
    local st=${line#GATE $1 }
    local status=${st%% *} frac=${st#* }
    echo "$status ${frac%/*} ${frac#*/}"
}
GATES_JSON=""
all_pass=1
add_gate() {   # add_gate <name> <status> <run> <failed> <sources>
    GATES_JSON+="${GATES_JSON:+,}
    \"$1\": {\"status\": \"$2\", \"tests_run\": $3, \"tests_failed\": $4, \"sources\": \"$5\"}"
    [ "$2" = PASS ] || { all_pass=0; REASONS+=("$1 $2"); }
}
test_gate() {   # test_gate <gate> <sources>
    local st p t
    read -r st p t < <(gate_line "$1")
    [ "${RC[test-program-realize]}" -eq 0 ] || st=FAIL
    add_gate "$1" "$st" "$t" "$((t - p))" "$2"
}
test_gate OMEGA_PROGRAM_REALIZE_PASS \
    "make test-program-realize (gate PROGRAM_REALIZE: omega_program_realize realizes + binds + executes named programs; builder/compose use it; 11 unsupported semantics fail closed with reasons)"
test_gate OMEGA_REALIZATION_PROGRAM_DRIVEN_PASS \
    "make test-program-realize (gate PROGRAM_DRIVEN: 600 programs pairwise distinct code + RealizationIds per and across machines, lift(sequential code) == body, schedule from machine, determinism, M14 3x-2 bytes preserved, legacy fixed-3x-2 synth rejected)"
test_gate OMEGA_REALIZATION_DIFFERENTIAL_PASS \
    "make test-program-realize (gate DIFFERENTIAL: native execution == independent semantic evaluator, 18 named + 582 seeded random pipelines, u8..u64, both canonical machines, boundary + random inputs)"
test_gate OMEGA_REALIZATION_TRIPLE_BIND_PASS \
    "make test-program-realize (gate TRIPLE_BIND: SEMANTIC_ID + MACHINE_ID + code -> REALIZATION_ID; rebinding with consistent triple, altered code, altered machine id, forged machine graph all rejected)"

r_run=0; r_fail=0; r_st=PASS
for t in "${REGRESSION[@]}"; do
    r_run=$((r_run + RUN[$t])); r_fail=$((r_fail + FAILED[$t]))
    { [ "${RC[$t]}" -eq 0 ] && [ "${FAILED[$t]}" -eq 0 ]; } || { r_st=FAIL; [ "${FAILED[$t]}" -eq 0 ] && r_fail=$((r_fail + 1)); }
done
add_gate OMEGA_REALIZATION_REGRESSION_PASS "$r_st" "$r_run" "$r_fail" "make ${REGRESSION[*]} (visor-qualification = run_qualification.sh)"

sanity=$(grep -m1 '^SANITY legacy_fixed_3x2_synth' "$prz_log" || echo "SANITY line missing")
case "$sanity" in *"PROGRAM_DRIVEN FAIL (expected)"*) ;; *) all_pass=0; REASONS+=("legacy sanity demo missing") ;; esac
fail_closed=$(grep '^FAIL_CLOSED' "$prz_log" || true)

# ---- tree stability ---------------------------------------------------------
head1=$(git rev-parse HEAD)
dirty1=$(git status --porcelain)
stable=true
{ [ "$head1" = "$head0" ] && [ "$dirty1" = "$dirty0" ]; } || { stable=false; REASONS+=("tree changed during the run"); }
tree_dirty=false; [ -z "$dirty0" ] || tree_dirty=true
verdict=OMEGA_PROGRAM_REALIZE_PASS
{ [ "$all_pass" -eq 1 ] && [ "$stable" = true ] && [ "$tree_dirty" = false ] && [ "$lib_ok" -eq 1 ] &&
  [ "$physics_head" = "$physics_pin" ] && [ "$(uname -m)" = aarch64 ]; } || verdict=OMEGA_PROGRAM_REALIZE_FAIL

json_str() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | awk 'BEGIN{ORS=""} {if (NR>1) print "\\n"; print}'; }
reasons_json=""
for r in "${REASONS[@]}"; do reasons_json+="${reasons_json:+, }\"$(json_str "$r")\""; done

body=$(cat <<EOF2
{
  "schema": "omega-program-realize-qualification/1",
  "spec": "spec/program-realization.md",
  "pipeline": "canonical program body -> V0 semantic gate -> machine-aware schedule (MachineGraph chooses schedule only) -> AArch64 V0 bytes (aarch64_encoder) -> REALIZATION_ID = triple(SEMANTIC_ID, MACHINE_ID, code) -> verification (bindings, V0 structural, native == semantic evaluator)",
  "run_id": "$run_id",
  "candidate_commit": "$head0",
  "candidate_tree": "$tree0",
  "tree_dirty": $tree_dirty,
  "tree_stable_during_run": $stable,
  "host": {"uname_m": "$(uname -m)", "uname_r": "$(uname -r)", "product_name": "$(json_str "$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)")"},
  "hardware_scope": "host CPU only (native AArch64 execution of generated code); no GPU, no silicon, no energy measurement",
  "aienos": {"lock": "$aienos_pin", "source": "git archive $aienos_pin native/capability (tree $aienos_src_tree)", "lib_sha256": "$lib_sha", "lib_built": $([ "$lib_ok" -eq 1 ] && echo true || echo false)},
  "physics": {"lock": "$physics_pin", "checkout_head": "$physics_head"},
  "gates": {$GATES_JSON
  },
  "legacy_sanity_demo": "$(json_str "$sanity")",
  "fail_closed": "$(json_str "$fail_closed")",
  "suites": [$SUITES_JSON
  ],
  "not_run": [
    "GPU / silicon targets (test-m12, test-m15, test-m17, test-m18, test-m19, r12 silicon seat, r13/r14 living runs, R15)",
    "suites CI runs that read the package energy counter (test-semantic-comm, test-cognitive-routing, test-sem-incremental, test-workflow-fusion, test-plan-reuse): not run here (no energy measurements); CI runs them"
  ],
  "non_claims": [
    "V0 unary constant-step chains only (ADD SUB MUL AND OR, unsigned u8..u64, <= 64 steps). DIV, signed, bitvector, conversions, calls, control flow fail closed.",
    "The differential samples 84-116 inputs per program; it is not an equivalence proof.",
    "No optimisation or measured cost: the schedule is a policy chosen from the canonical MachineGraph profiles (assumed, not observed hardware).",
    "Canonical program realization ids (OMG_R0) now bind the program id; historical receipts are not rewritten."
  ],
  "verdict": "$verdict",
  "verdict_reasons": [$reasons_json]
}
EOF2
)
mkdir -p "$OUTDIR"
tmp=$(mktemp "$OUTDIR/.tmp-XXXXXX")
printf '%s\n' "$body" > "$tmp"
digest=$(sha256sum "$tmp" | awk '{print $1}')
final=$OUTDIR/$digest.json
if [ -e "$final" ]; then rm -f "$tmp"; else mv "$tmp" "$final"; fi
chmod 0644 "$final"
grep -E '"OMEGA_[A-Z_]+_PASS"' "$final" | sed 's/^ *//'
echo "RECEIPT $final"
echo "VERDICT $verdict"
[ "$verdict" = OMEGA_PROGRAM_REALIZE_PASS ]
