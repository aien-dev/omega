#!/bin/bash
# r15_receipt.sh -- write the R15 receipt (spec/r15-performance-proof.md §12,
# §13, §14) from one qualification run directory.
#
#   tools/r15_receipt.sh <raw-dir> <out-dir> [candidate-commit] [reruns.json]
#
# Inputs, all read, none changed:
#   <raw-dir>/summary.json   the reducer output (every metric, comparison, CI,
#                            gate threshold and gate outcome)
#   <raw-dir>/machine.json   hardware identity and commits (§13), written by
#                            tools/r15_qualify.sh at run time
#   <raw-dir>/SHA256SUMS     the raw file digests
#   candidate-commit         the commit proposed for R15 (default: env
#                            OMEGA_CANDIDATE_COMMIT; empty = not candidate-bound)
#   reruns.json              the §14 correctness reruns (+ §16 item 9 SEQ parity)
#                            as one JSON object, e.g. {"R3":"PASS",...}; absent =
#                            not done
#
# Output: <out-dir>/<sha256>.json, schema AIEN_RX_R15_REACTION_PERFORMANCE_V1,
# named by the SHA-256 of its own bytes. The path is printed on stdout.
#
# No number is computed or edited here: summary.json and machine.json are
# embedded byte for byte. The writer only decides the outcome:
#   PASS only if every gate G1-G16 is PASS (all_gates_pass true), the raw
#   directory verifies against SHA256SUMS and the reducer's raw digest, the
#   run is candidate-bound (run commit == candidate, clean tree), silicon was
#   observed, aienos and physics commits are recorded and equal the pins in
#   aienos.lock/physics.lock at the run commit, and every required correctness
#   rerun is PASS. Anything else is FAIL, with the reasons listed.
# Shell and coreutils only (sha256sum, grep, sed, git). No jq, no Python.
set -u

RAW=${1:?raw run directory}
OUTDIR=${2:?output directory}
CANDIDATE=${3:-${OMEGA_CANDIDATE_COMMIT:-}}
RERUNS=${4:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)

SUM=$RAW/summary.json
MACH=$RAW/machine.json
for f in "$SUM" "$MACH" "$RAW/SHA256SUMS"; do
    [ -s "$f" ] || { echo "r15_receipt: missing $f" >&2; exit 2; }
done
mkdir -p "$OUTDIR" || exit 2

REQUIRED_RERUNS="R3 R7 R8 R9 R10 R11 R12_host R12_silicon R13_host R13_silicon R14_host R14_silicon R15_SEQ_parity"

# ---- small readers (flat keys only; the inputs are written by our own tools)
mstr() { grep -o "\"$1\":\"[^\"]*\"" "$MACH" | head -1 | sed 's/^"[^"]*":"//; s/"$//'; }
mraw() { grep -o "\"$1\":[^,}]*" "$MACH" | head -1 | sed 's/^"[^"]*"://'; }
sraw() { grep -o "\"$1\":[^,}]*" "$SUM" | tail -1 | sed 's/^"[^"]*"://'; }
jstr() { printf '"%s"' "$(printf '%s' "$1" | tr -d '"\\' | tr '\n' ' ')"; }

REASONS=()
fail() { REASONS+=("$1"); }

RUN_ID=$(mstr run_id)
MODE=$(mstr mode)
RUN_COMMIT=$(mstr candidate_commit)          # qualify.sh records HEAD at run time here
TREE_DIRTY=$(mraw tree_dirty)
AIENOS=$(mstr aienos_commit)
PHYSICS=$(mstr physics_commit)
REDUCER_SRC=$(mstr reducer_source_sha256)
BINARIES=$(grep -o '"binaries":{[^}]*}' "$MACH" | sed 's/^"binaries"://')
SCHEMA_IN=$(grep -o '"schema":"[^"]*"' "$SUM" | head -1 | sed 's/.*:"//; s/"$//')
ALL_PASS=$(sraw all_gates_pass)
DIGEST_CLAIMED=$(grep -o '"raw_digest_sha256_of_SHA256SUMS":"[0-9a-f]*"' "$SUM" | sed 's/.*:"//; s/"$//')
DIGEST_ACTUAL=$(sha256sum "$RAW/SHA256SUMS" | cut -d' ' -f1)
GATES=$(grep -o '"gates":\[[^]]*\]' "$SUM" | sed 's/^"gates"://')

[ "$SCHEMA_IN" = AIEN_RX_R15_SUMMARY_V1 ] || fail "summary.json schema is '$SCHEMA_IN', not AIEN_RX_R15_SUMMARY_V1"
[ -n "$GATES" ] || fail "summary.json has no gate table"

# ---- gates (§11): every one of G1-G16 present and PASS
NGATES=0
for g in $(seq 1 16); do
    ent=$(printf '%s' "$GATES" | grep -o "{\"id\":\"G$g\"[^}]*}")
    if [ -z "$ent" ]; then fail "gate G$g missing from summary.json"; continue; fi
    NGATES=$((NGATES + 1))
    out=$(printf '%s' "$ent" | grep -o '"outcome":"[A-Z]*"' | sed 's/.*:"//; s/"$//')
    [ "$out" = PASS ] || fail "gate G$g $out: $(printf '%s' "$ent" | grep -o '"note":"[^"]*"' | sed 's/^"note":"//; s/"$//')"
done
[ "$ALL_PASS" = true ] || fail "reducer reports all_gates_pass=$ALL_PASS"

# ---- raw evidence integrity (§12)
RAW_FILES_OK=false
if (cd "$RAW" && sha256sum --quiet -c SHA256SUMS >/dev/null 2>&1); then RAW_FILES_OK=true
else fail "raw files do not verify against SHA256SUMS"; fi
DIGEST_OK=false
if [ -n "$DIGEST_CLAIMED" ] && [ "$DIGEST_CLAIMED" = "$DIGEST_ACTUAL" ]; then DIGEST_OK=true
else fail "raw digest in summary.json ($DIGEST_CLAIMED) != SHA-256 of SHA256SUMS ($DIGEST_ACTUAL)"; fi

# ---- candidate binding
BOUND=false
if [ -z "$CANDIDATE" ]; then fail "no candidate commit given"
elif [ "$CANDIDATE" != "$RUN_COMMIT" ]; then fail "run commit $RUN_COMMIT is not the candidate $CANDIDATE"
elif [ "$TREE_DIRTY" != false ]; then fail "run tree was dirty"
else BOUND=true; fi

SILICON=false
if [ "$MODE" = silicon ]; then SILICON=true; else fail "run mode is '$MODE', not silicon"; fi

# ---- pinned dependencies (aienos.lock / physics.lock at the run commit)
AIENOS_LOCK=$(git -C "$HERE" show "$RUN_COMMIT:aienos.lock" 2>/dev/null | tr -d '[:space:]')
PHYSICS_LOCK=$(git -C "$HERE" show "$RUN_COMMIT:physics.lock" 2>/dev/null | tr -d '[:space:]')
if [ -z "$AIENOS" ]; then fail "aienos_commit not recorded in machine.json"
elif [ "$AIENOS" != "$AIENOS_LOCK" ]; then fail "aienos_commit $AIENOS != aienos.lock $AIENOS_LOCK"; fi
if [ -z "$PHYSICS" ]; then fail "physics_commit not recorded in machine.json"
elif [ "$PHYSICS" != "$PHYSICS_LOCK" ]; then fail "physics_commit $PHYSICS != physics.lock $PHYSICS_LOCK"; fi

# ---- correctness reruns (§14, §16 item 9)
if [ -n "$RERUNS" ] && [ -s "$RERUNS" ]; then
    RERUN_JSON=$(tr -d '\n' < "$RERUNS")
    for k in $REQUIRED_RERUNS; do
        printf '%s' "$RERUN_JSON" | grep -q "\"$k\":\"PASS\"" || fail "correctness rerun $k not PASS"
    done
else
    RERUN_JSON='{"status":"not done"}'
    fail "correctness reruns (§14) not recorded"
fi

OUTCOME=FAIL
[ ${#REASONS[@]} -eq 0 ] && OUTCOME=PASS

# ---- regressions: every failed gate, with its value and threshold
REGR=$(printf '%s' "$GATES" | grep -o '{"id":"G[0-9]*"[^}]*"outcome":"FAIL"[^}]*}' \
    | sed 's/}$/,"justification":"none: a failed gate means no PASS (spec §11)"}/' | paste -sd, -)

reasons_json() {
    local first=1 r
    printf '['
    for r in "${REASONS[@]}"; do
        [ $first = 1 ] || printf ','
        first=0
        jstr "$r"
    done
    printf ']'
}

TMP=$(mktemp "$OUTDIR/.r15-receipt.XXXXXX") || exit 2
{
    printf '{\n'
    printf '  "schema": "AIEN_RX_R15_REACTION_PERFORMANCE_V1",\n'
    printf '  "run_id": %s,\n' "$(jstr "$RUN_ID")"
    printf '  "outcome": "%s",\n' "$OUTCOME"
    printf '  "outcome_reasons": %s,\n' "$(reasons_json)"
    printf '  "candidate_commit": %s,\n' "$(jstr "$CANDIDATE")"
    printf '  "run_commit": %s,\n' "$(jstr "$RUN_COMMIT")"
    printf '  "candidate_bound": %s,\n' "$BOUND"
    printf '  "tree_dirty": %s,\n' "${TREE_DIRTY:-null}"
    printf '  "silicon_observed": %s,\n' "$SILICON"
    printf '  "aienos_commit": %s,\n' "$(jstr "$AIENOS")"
    printf '  "aienos_lock": %s,\n' "$(jstr "$AIENOS_LOCK")"
    printf '  "physics_commit": %s,\n' "$(jstr "$PHYSICS")"
    printf '  "physics_lock": %s,\n' "$(jstr "$PHYSICS_LOCK")"
    printf '  "benchmark_binaries_sha256": %s,\n' "${BINARIES:-null}"
    printf '  "raw_digest_sha256": %s,\n' "$(jstr "$DIGEST_ACTUAL")"
    printf '  "raw_digest_matches_reducer": %s,\n' "$DIGEST_OK"
    printf '  "raw_files_verified": %s,\n' "$RAW_FILES_OK"
    printf '  "reducer_source_sha256": %s,\n' "$(jstr "$REDUCER_SRC")"
    printf '  "legacy_status": "NOT DIRECTLY COMPARABLE (spec §2): sovereign-core run_until_complete/step is LLM batched decode; aegis-runtime execute_task is an LLM tool-calling loop; the baseline is the sequential control SEQ",\n'
    printf '  "seq_selector_symbol": "rx_world_init_native_sequential_reference",\n'
    printf '  "nodigest_macro": {"name": "RX_MEASURE_NO_CAUSAL_DIGEST", "defined_in_production_binaries": false, "used_by": "RES-1-NODIGEST measurement build only"},\n'
    printf '  "gates_present": %s,\n' "$NGATES"
    printf '  "all_gates_pass": %s,\n' "${ALL_PASS:-null}"
    printf '  "gates": %s,\n' "${GATES:-[]}"
    printf '  "correctness_reruns_required": %s,\n' \
        "[$(for k in $REQUIRED_RERUNS; do jstr "$k"; printf ','; done | sed 's/,$//')]"
    printf '  "correctness_reruns": %s,\n' "$RERUN_JSON"
    printf '  "regressions": [%s],\n' "$REGR"
    printf '  "not_claimed": ["R16", "whole-project performance beyond the measured workloads", "LLM decode or agent-loop performance (the legacy loops, spec §2)", "general cognition", "open-ended synthesis", "energy of anything outside the measured windows"],\n'
    printf '  "hardware_identity": '
    tr -d '\n' < "$MACH"
    printf ',\n  "summary": '
    tr -d '\n' < "$SUM"
    printf '\n}\n'
} > "$TMP"
SHA=$(sha256sum "$TMP" | cut -d' ' -f1)
mv "$TMP" "$OUTDIR/$SHA.json" || exit 2
echo "$OUTDIR/$SHA.json"
[ "$OUTCOME" = PASS ] && exit 0 || exit 1
