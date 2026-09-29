#!/bin/bash
# effect_cap64_receipt.sh -- qualification receipt for the 64-bit effect
# capability generation migration (spec/effect-cap64-migration.md).
#
#   tools/effect_cap64_receipt.sh [out-dir]      (default: evidence/EFFECT_CAP64)
#
# Runs, on the committed tree only:
#   - the pinned AIENOS capability library, rebuilt from `git archive` of the
#     commit in aienos.lock (AIENOS_REPO, default ~/workspace/aienos-repo)
#   - make test-effect-cap64                         (4 gates, physics-free)
#   - make test-visor-core, test-visor-world, test-visor-authority
#   - the non-energy host regression suites CI runs: test (M4 omegatool),
#     r3 r7 r8 r9 r10 r11 r12 action-graph state-projection capability-query
#     typed-results
#   - a grep for 32-bit truncation of an effect capability generation
# Every make is wrapped in `flock $BENCH_LOCK`. Nothing under evidence/ is
# rewritten: the receipt is a NEW file named by the SHA-256 of its own bytes.
# The receipt says FAIL (and lists why) when any suite fails, the tree is
# dirty or changes during the run, or the pins do not match.
# Shell and coreutils only (git, sha256sum, grep, sed, awk, tar, make).
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 2
OUTDIR=${1:-evidence/EFFECT_CAP64}
AIENOS_REPO=${AIENOS_REPO:-$HOME/workspace/aienos-repo}
PHYSICS_DIR=${PHYSICS_DIR:-$HOME/workspace/physics-r13}
BENCH_LOCK=${BENCH_LOCK:-$HOME/workspace/.argus-bench.lock}
Q=build/cap64q
LOGS=$Q/logs
REASONS=()

head0=$(git rev-parse HEAD)
tree0=$(git rev-parse 'HEAD^{tree}')
dirty0=$(git status --porcelain)
[ -z "$dirty0" ] || REASONS+=("tree dirty at start")
run_id=$(date -u +%Y%m%dT%H%M%SZ)

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
hdr_sha=$( [ -f "$Q/aienos/native/capability/aienos_capability.h" ] &&
           sha256sum "$Q/aienos/native/capability/aienos_capability.h" | awk '{print $1}' || echo null)

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

suite() {    # suite <name> <out-dir> <make args...>
    local name=$1 out=$2
    shift 2
    local log=$LOGS/$name.log
    flock "$BENCH_LOCK" env OMEGA_CANDIDATE_COMMIT="$head0" AIENOS_COMMIT="$aienos_pin" \
        PHYSICS_COMMIT="$physics_pin" make OUT_DIR="$out" "$@" "$name" > "$log" 2>&1
    local rc=$?
    local src
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

PF=(PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 AIENOS_R7_DIR="$Q/aienos")
RT=(PHYSICS_DIR="$PHYSICS_DIR" AIENOS_R7_DIR="$Q/aienos" VISOR_AUTH_CAP_LIB="$LIB")

suite test-effect-cap64 "$Q/pf" "${PF[@]}"
suite test-visor-core "$Q/pf" "${PF[@]}"
suite test-visor-world "$Q/pf" "${RT[@]}"
suite test-visor-authority "$Q/pf" "${RT[@]}"
REGRESSION=(test test-r3 test-r7 test-r8 test-r9 test-r10 test-r11 test-r12 test-action-graph
            test-state-projection test-capability-query test-typed-results)
for t in "${REGRESSION[@]}"; do suite "$t" "$Q/reg" "${RT[@]}"; done

# ---- truncation grep --------------------------------------------------------
trunc_hits=$(grep -rnE '\(uint32_t\)[^;]*capability_generation|capability_generation[^;]*[^0-9]%u|uint32_t +(cap_gen|capability_generation)\b' \
    src tools/omega.c tests --include='*.c' --include='*.h' | grep -v 'src/omega_accelerator' || true)
trunc_n=$(printf '%s' "$trunc_hits" | grep -c . || true)
[ "$trunc_n" -eq 0 ] || REASONS+=("effect generation truncation grep: $trunc_n hit(s)")

# ---- gates ------------------------------------------------------------------
eff_log=$LOGS/test-effect-cap64.log
gate_from_effect() {   # -> "STATUS passed total"
    local line
    line=$(grep -E "^GATE $1 (PASS|FAIL) [0-9]+/[0-9]+$" "$eff_log" | tail -n 1)
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
for g in ROUNDTRIP IDENTITY STALE_REJECT; do
    read -r st p t < <(gate_from_effect "OMEGA_EFFECT_CAP64_${g}_PASS")
    extra=""
    if [ "$g" = ROUNDTRIP ]; then
        t=$((t + 1)); [ "$trunc_n" -eq 0 ] && p=$((p + 1)) || st=FAIL
        extra=" + truncation grep"
    fi
    [ "${RC[test-effect-cap64]}" -eq 0 ] || st=FAIL
    add_gate "OMEGA_EFFECT_CAP64_${g}_PASS" "$st" "$t" "$((t - p))" "make test-effect-cap64$extra"
done
read -r st p t < <(gate_from_effect OMEGA_EFFECT_CAP64_AUTHORITY_PASS)
va_run=${RUN[test-visor-authority]}; va_f=${FAILED[test-visor-authority]}
link_ok=0; grep -q '^OMEGA_VISOR_AUTHORITY_ISOLATION_PASS$' "$LOGS/test-visor-core.log" && link_ok=1
a_run=$((t + va_run + 1)); a_fail=$((t - p + va_f + 1 - link_ok))
a_st=PASS
{ [ "$st" = PASS ] && [ "${RC[test-visor-authority]}" -eq 0 ] && [ "$va_f" -eq 0 ] && [ "$va_run" -gt 0 ] &&
  [ "$link_ok" -eq 1 ]; } || a_st=FAIL
add_gate OMEGA_EFFECT_CAP64_AUTHORITY_PASS "$a_st" "$a_run" "$a_fail" \
    "make test-effect-cap64 (authority, real AIENOS lib) + make test-visor-authority + visor-authority-check (in test-visor-core)"
r_run=0; r_fail=0; r_st=PASS
for t in test-visor-core test-visor-world "${REGRESSION[@]}"; do
    r_run=$((r_run + RUN[$t])); r_fail=$((r_fail + FAILED[$t]))
    [ "${RC[$t]}" -eq 0 ] && [ "${FAILED[$t]}" -eq 0 ] || { r_st=FAIL; [ "${FAILED[$t]}" -eq 0 ] && r_fail=$((r_fail + 1)); }
done
add_gate OMEGA_EFFECT_CAP64_REGRESSION_PASS "$r_st" "$r_run" "$r_fail" \
    "make test-visor-core test-visor-world ${REGRESSION[*]} (tests_run = PASS n/m lines, omegatool TOTAL GATES, or the suite's own receipt checks/failures; exit status always required)"

# ---- tree stability ---------------------------------------------------------
head1=$(git rev-parse HEAD)
dirty1=$(git status --porcelain)
stable=true
{ [ "$head1" = "$head0" ] && [ "$dirty1" = "$dirty0" ]; } || { stable=false; REASONS+=("tree changed during the run"); }
tree_dirty=false; [ -z "$dirty0" ] || tree_dirty=true
verdict=OMEGA_EFFECT_CAP64_PASS
{ [ "$all_pass" -eq 1 ] && [ "$stable" = true ] && [ "$tree_dirty" = false ] && [ "$lib_ok" -eq 1 ] &&
  [ "$physics_head" = "$physics_pin" ]; } || verdict=OMEGA_EFFECT_CAP64_FAIL

json_str() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' | awk 'BEGIN{ORS=""} {if (NR>1) print "\\n"; print}'; }
reasons_json=""
for r in "${REASONS[@]}"; do reasons_json+="${reasons_json:+, }\"$(json_str "$r")\""; done

body=$(cat <<EOF
{
  "schema": "omega-effect-cap64-qualification/1",
  "spec": "spec/effect-cap64-migration.md",
  "run_id": "$run_id",
  "candidate_commit": "$head0",
  "candidate_tree": "$tree0",
  "tree_dirty": $tree_dirty,
  "tree_stable_during_run": $stable,
  "host": {"uname_m": "$(uname -m)", "uname_r": "$(uname -r)", "product_name": "$(json_str "$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)")"},
  "hardware_scope": "host-only (CPU); no GPU, no silicon, no energy measurement",
  "aienos": {"lock": "$aienos_pin", "source": "git archive $aienos_pin native/capability (tree $aienos_src_tree)", "header_sha256": "$hdr_sha", "lib_sha256": "$lib_sha", "lib_built": $([ "$lib_ok" -eq 1 ] && echo true || echo false)},
  "physics": {"lock": "$physics_pin", "checkout_head": "$physics_head"},
  "gates": {$GATES_JSON
  },
  "suites": [$SUITES_JSON
  ],
  "truncation_grep": {"hits": $trunc_n, "detail": "$(json_str "$trunc_hits")"},
  "not_run": [
    "GPU / silicon targets (test-m12, test-m15, test-m17, test-m18, test-m19, r12 silicon seat, r13/r14 living runs, R15)",
    "suites CI runs that read the package energy counter (test-semantic-comm, test-cognitive-routing, test-sem-incremental, test-workflow-fusion, test-plan-reuse): not run here (no energy measurements); CI runs them"
  ],
  "non_claims": [
    "The accelerator OmegaEffectIntent / OmegaEffectReceipt (src/omega_accelerator.h) still carry a 32-bit capability_generation; they belong to the physics accelerator port model, not AIENOS, and are unchanged.",
    "Visor JSON prints the generation as an exact decimal integer; consumers parsing JSON numbers as doubles lose precision above 2^53.",
    "No effect is executed; operation_code/resource_class -> AIENOS rights/resource is still a test convention.",
    "Old (version 0x01) effect objects are refused, not migrated; none exist in this repository."
  ],
  "verdict": "$verdict",
  "verdict_reasons": [$reasons_json]
}
EOF
)
mkdir -p "$OUTDIR"
tmp=$(mktemp "$OUTDIR/.tmp-XXXXXX")
printf '%s\n' "$body" > "$tmp"
digest=$(sha256sum "$tmp" | awk '{print $1}')
final=$OUTDIR/$digest.json
if [ -e "$final" ]; then rm -f "$tmp"; else mv "$tmp" "$final"; fi
chmod 0644 "$final"
grep -E '"OMEGA_EFFECT_CAP64_[A-Z_]+_PASS"' "$final" | sed 's/^ *//'
echo "RECEIPT $final"
echo "VERDICT $verdict"
[ "$verdict" = OMEGA_EFFECT_CAP64_PASS ]
