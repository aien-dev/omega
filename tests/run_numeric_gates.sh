#!/bin/bash
# tests/run_numeric_gates.sh -- M19R Gate 5 (OMEGA-NUMERIC-0) qualification.
#
#   tests/run_numeric_gates.sh --omega-candidate SHA --physics-candidate SHA
#                              [--physics-dir DIR] [--record]
#
# Needs the GB10: the test binary launches kernels on the chip. The build and
# the run happen under /tmp/aien-gb10.lock.
#
# Checks, in order:
#   1. both candidates are explicit full 40-hex SHAs equal to HEAD of a clean
#      tree (omega = this repo, physics = --physics-dir, default ../physics);
#   2. physics.lock names the physics candidate, and the physics checkout is at
#      that commit (the dependency comes from the pin file, never a fixed path);
#   3. the pinned physics commit has the forge hardware probe (forge/); if not
#      the run stops and says so (moving the pin is an owner decision);
#   4. clean build into build/qual-runs/<run-id>/ with -ffp-contract=off, no
#      libm and no CUDA symbols in the binary;
#   5. the binary runs; every expected test ID printed exactly once as PASS,
#      no SKIP, a GB10 parity line with zero mismatches for every encoded op,
#      and a hardware descriptor probed from the device (a fake one is refused);
#   6. candidates re-checked and evidence/ unchanged at the end.
#
# Output: build/qual-runs/<run-id>/{build.log,gate5.log,run.json} always;
# receipt-preview.json when every check passed; with --record the permanent
# receipt evidence/OMEGA-NUMERIC-0/<receipt_digest>.json (created exclusively,
# mode 0444, never overwritten). No receipt is written on any failure.
# The historical file evidence/m19r_gate5_omega_numeric_evidence.json is not
# touched; its hash is recorded as the predecessor.
# Exit 0 on PASS, 1 on failure, 2 on bad arguments.
#
# Functions can be sourced (tools/test_numeric_qualify.sh does); main only
# runs when the file is executed. Shell + coreutils + git + jq + gcc. No Python.

NUM_SELF=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
# shellcheck source=../tools/m19r_qualify.sh
. "$(dirname "$NUM_SELF")/../tools/m19r_qualify.sh"
M19R_PROG=run_numeric_gates.sh

NUM_SUITE=gate5
NUM_HISTORICAL=evidence/m19r_gate5_omega_numeric_evidence.json
# Test IDs the chip build must print, each exactly once, each PASS.
NUM_EXPECTED_IDS="FPCR_RNE_NO_FTZ_REQUIRED PROVENANCE_MATCHES_EXECUTOR
NOT_ENCODED_OPS_REFUSED_BEFORE_SUBMISSION FP32_SIMT_OPCODES_ENCODED
OMEGA_MATH_SEQUENCES_QUALIFIED WARP_REDUCTION_ORDER_DECLARED
CPU_TIER_EQUALS_REFERENCE CPU_TIER_SUBNORMALS_PRESERVED CPU_GB10_BIT_PARITY
SUBNORMALS_PRESERVED_NO_FTZ MUFU_SEED_ONLY_NOT_COMPARED
EDGE_CLASS_BEHAVIOR_VERIFIED HARDWARE_DESCRIPTOR_PROBED
NEG_FTZ_DETECTED_AND_REJECTED NEG_UNORDERED_REDUCTION_DIVERGENCE_CAUGHT
NEG_RAW_MUFU_APPROX_REJECTED_WITHOUT_REFINEMENT NEG_UNKNOWN_OPCODE_FAILS_CLOSED
NEG_OPCODE_PROVENANCE_INTEGRITY_VERIFIED NEG_NONDEFAULT_FPCR_REFUSED
NEG_COMPARATOR_CATCHES_ONE_BIT"
# Ops the executor encodes for GB10; each needs a GB10 parity line.
NUM_ENCODED_OPS="FADD FSUB FMUL FFMA FSETP_SEL FSEL FMNMX_MIN FMNMX_MAX I2FP F2I
MUFU_RCP MUFU_RSQ SHFL_DOWN"

# num_check_physics OMEGA PHYSICS_DIR PHYSICS_CAND -- the physics dependency
# is the commit in physics.lock; the checkout must be at it and carry forge/.
num_check_physics() {
    local omega=$1 physics=$2 cand=$3 locked head f
    [ -r "$omega/physics.lock" ] || m19r_fail "cannot read $omega/physics.lock" || return 1
    locked=$(m19r_strip "$(cat "$omega/physics.lock")")
    [[ $locked =~ ^[0-9a-fA-F]{40}$ ]] ||
        m19r_fail "physics.lock does not hold a full 40-hex commit" || return 1
    [ "${locked,,}" = "${cand,,}" ] ||
        m19r_fail "physics.lock pins $locked but --physics-candidate is $cand" || return 1
    [ -d "$physics" ] ||
        m19r_fail "physics checkout $physics does not exist (pinned commit $locked)" || return 1
    m19r_git "$physics" rev-parse HEAD ||
        m19r_fail "physics checkout $physics is not a git repository" || return 1
    head=$M19R_GIT
    [ "${head,,}" = "${locked,,}" ] ||
        m19r_fail "physics checkout $physics is at $head but physics.lock pins $locked" || return 1
    m19r_must_candidate "$physics" "$cand" || return 1
    for f in forge/forge_descriptor.h forge/forge_descriptor.c forge/forge_realize.c \
             sha256_clean.c nvrm/nvrm.c m16/m16_native.c; do
        [ -f "$physics/$f" ] || m19r_fail "pinned physics commit $locked has no $f; Gate 5 needs the forge hardware probe, so physics.lock must move to a commit that has it (owner decision, not done by this script)" || return 1
    done
}

# num_build OMEGA PHYSICS OUT LOG -- build the chip test binary.
num_build() {
    local omega=$1 p=$2 out=$3 log=$4 nv
    nv=$p/third_party/nvidia-open-580.173.02
    (cd "$omega" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off \
        -Isrc -I"$p/forge" -I"$p/nvrm" -I"$p/m16" \
        -I"$nv/src/common/sdk/nvidia/inc" -I"$nv/kernel-open/common/inc" \
        -I"$nv/kernel-open/nvidia-uvm" -I"$nv/src/nvidia/arch/nvalloc/unix/include" \
        -o "$out" tests/test_omega_numeric.c src/omega_numeric.c src/omega_numeric_gb10.c \
        src/omega_numeric_provenance.c src/omega_blackwell_codegen.c \
        src/omega_blackwell_encoder.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c \
        src/forge_realization.c src/aegis_verification.c src/sha256.c \
        "$p/forge/forge_descriptor.c" "$p/forge/forge_realize.c" "$p/sha256_clean.c" \
        "$p/nvrm/nvrm.c" "$p/m16/m16_native.c") > "$log" 2>&1 ||
        m19r_fail "build failed; see $log" || return 1
    if nm -u "$out" | grep -Eq '\b(sqrtf?|expf?|logf?|powf?|fmaf?|sinf?|cosf?|roundf?|fabsf?)\b'; then
        m19r_fail "libm math symbols found in $out"; return 1
    fi
    if nm -u "$out" | grep -Eq "$M19R_CUDA_RE"; then
        m19r_fail "CUDA symbols found in $out"; return 1
    fi
}

# num_check_log LOG -- validate the gate log. Sets NUM_TSV (events file),
# NUM_HWDESC, NUM_HWDIGEST, NUM_PARITY (JSON array of GB10 parity lines).
num_check_log() {
    local log=$1 id n line op hw
    [ -r "$log" ] || m19r_fail "cannot read gate log $log" || return 1
    if grep -Eq '^[[:space:]]*\[SKIP\]' "$log"; then
        m19r_fail "gate log has SKIP results; a qualifying run executes every test"; return 1
    fi
    NUM_TSV=$M19R_TMP/gate5.tsv
    m19r_events "$log" "$NUM_SUITE" > "$NUM_TSV"
    for id in $NUM_EXPECTED_IDS; do
        n=$(awk -F'\t' -v i="$id" '$2 == i' "$NUM_TSV" | grep -c .)
        [ "$n" -eq 1 ] || m19r_fail "expected test $id printed $n times (need exactly 1)" || return 1
    done
    n=$(grep -c . "$NUM_TSV")
    [ "$n" -eq "$(printf '%s\n' $NUM_EXPECTED_IDS | grep -c .)" ] ||
        m19r_fail "gate log has $n results; the manifest lists $(printf '%s\n' $NUM_EXPECTED_IDS | grep -c .)" || return 1
    m19r_require_passed < "$NUM_TSV" || return 1

    hw=$(m19r_tagged_json "$log" OMEGA_NUMERIC_HWDESC_JSON) || return 1
    [ "$(printf '%s\n' "$hw" | grep -c .)" -eq 1 ] ||
        m19r_fail "expected exactly one hardware descriptor line" || return 1
    [ "$(printf '%s' "$hw" | jq -r '.source')" = FORGE_PROBE ] ||
        m19r_fail "hardware descriptor is not a device probe (source $(printf '%s' "$hw" | jq -c '.source')); refusing" || return 1
    [ "$(printf '%s' "$hw" | jq -r '.fake // false')" = false ] ||
        m19r_fail "hardware descriptor is marked fake; refusing" || return 1
    NUM_HWDIGEST=$(printf '%s' "$hw" | jq -r '.descriptor_digest')
    [[ $NUM_HWDIGEST =~ ^[0-9a-f]{64}$ ]] && [ "$NUM_HWDIGEST" != "$(printf '0%.0s' {1..64})" ] ||
        m19r_fail "hardware descriptor digest is missing or all zero" || return 1
    NUM_HWDESC=$hw

    NUM_PARITY=$(m19r_tagged_json "$log" OMEGA_NUMERIC_PARITY_JSON | jq -sc '[.[] | select(.tier == "gb10")]') ||
        m19r_fail "cannot read GB10 parity lines" || return 1
    for op in $NUM_ENCODED_OPS; do
        line=$(printf '%s' "$NUM_PARITY" | jq -c --arg o "$op" '[.[] | select(.op == $o)]')
        [ "$(printf '%s' "$line" | jq 'length')" -ge 1 ] ||
            m19r_fail "no GB10 parity line for $op" || return 1
        [ "$(printf '%s' "$line" | jq '[.[] | select(has("error")
                or (if .compare == "SEED_BOUND" then (.out_of_bound != 0 or .checked == 0)
                    else (.mismatches != 0 or .checked == 0 or .checked != .n) end))] | length')" -eq 0 ] ||
            m19r_fail "GB10 parity for $op has an error or mismatches" || return 1
    done
    [ "$(printf '%s' "$NUM_PARITY" | jq --arg l "$NUM_ENCODED_OPS" '[.[].op] - ($l | split("\\s+"; null)) | length')" -eq 0 ] ||
        m19r_fail "GB10 parity lines name an op outside the encoded set" || return 1
}

# num_receipt OMEGA LOG -- build the receipt from a checked log and write the
# preview (always) and, with NUM_RECORD=1, the permanent receipt. Needs
# NUM_OMEGA_CAND NUM_PHYSICS_CAND NUM_RUN_COMMIT NUM_BINARY_SHA NUM_RUN_ID
# NUM_TS NUM_RUN_DIR.
num_receipt() {
    local omega=$1 log=$2 manifest manifest_digest counts body digest receipt pred
    num_check_log "$log" || return 1
    [ "${NUM_RUN_COMMIT,,}" = "${NUM_OMEGA_CAND,,}" ] ||
        m19r_fail "run commit $NUM_RUN_COMMIT differs from candidate $NUM_OMEGA_CAND" || return 1
    [[ $NUM_BINARY_SHA =~ ^[0-9a-f]{64}$ ]] || m19r_fail "missing binary digest" || return 1
    [[ $NUM_TS =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z$ ]] || m19r_fail "missing UTC timestamp" || return 1
    pred=null
    [ ! -f "$omega/$NUM_HISTORICAL" ] || pred="\"$(m19r_sha_file "$omega/$NUM_HISTORICAL")\""
    manifest="{\"command\":\"tests/test_omega_numeric\",\"expected_test_ids\":$(printf '%s\n' $NUM_EXPECTED_IDS | jq -Rn '[inputs]'),\"encoded_ops\":$(printf '%s\n' $NUM_ENCODED_OPS | jq -Rn '[inputs]')}"
    manifest_digest=$(printf '%s' "$manifest" | "$JSON_CANON" --sha256) || return 1
    counts=$(m19r_observed_counts < "$NUM_TSV")
    body="{\"schema\":\"AIEN_OMEGA_NUMERIC_0_V1\",\"gate\":\"M19R_GATE5_OMEGA_NUMERIC_0\""
    body+=",\"run_id\":$(m19r_jstr "$NUM_RUN_ID"),\"timestamp_utc\":\"$NUM_TS\""
    body+=",\"candidate_git_commit\":\"${NUM_OMEGA_CAND,,}\",\"run_git_commit\":\"${NUM_RUN_COMMIT,,}\""
    body+=",\"physics_candidate_git_commit\":\"${NUM_PHYSICS_CAND,,}\""
    body+=",\"physics_lock\":\"$(m19r_strip "$(cat "$omega/physics.lock")" | tr 'A-F' 'a-f')\""
    body+=",\"candidate_trees_clean\":{\"omega\":true,\"physics\":true}"
    body+=",\"candidate_binary_sha256\":\"$NUM_BINARY_SHA\""
    body+=",\"test_manifest_sha256\":\"$manifest_digest\",\"test_manifest\":$manifest"
    body+=",\"test_results\":$(m19r_events_json < "$NUM_TSV")"
    body+=",\"observed_test_count\":$(jq -n --argjson c "$counts" '$c.completed')"
    body+=",\"observed_pass_count\":$(jq -n --argjson c "$counts" '$c.passed')"
    body+=",\"observed_fail_count\":$(jq -n --argjson c "$counts" '$c.failed')"
    body+=",\"hardware_descriptor\":$NUM_HWDESC,\"hardware_descriptor_digest\":\"$NUM_HWDIGEST\""
    body+=",\"gb10_parity\":$NUM_PARITY"
    body+=",\"predecessor_historical_gate5_sha256\":$pred,\"zero_libm_zero_libcuda\":true}"
    digest=$(printf '%s' "$body" | "$JSON_CANON" --sha256) ||
        m19r_fail "receipt body is not valid JSON" || return 1
    receipt="{\"receipt_digest\":\"$digest\",${body#\{}"
    printf '%s' "$receipt" | "$JSON_CANON" --pretty > "$NUM_RUN_DIR/receipt-preview.json" ||
        m19r_fail "cannot write receipt preview" || return 1
    NUM_DIGEST=$digest
    if [ "${NUM_RECORD:-0}" = 1 ]; then
        NUM_PERMANENT=$omega/evidence/OMEGA-NUMERIC-0/$digest.json
        printf '%s' "$receipt" | m19r_write_immutable_receipt "$NUM_PERMANENT" || return 1
    fi
}

num_qualify() {
    local omega=$M19R_OMEGA bin before after rc
    m19r_must_candidate "$omega" "$NUM_OMEGA_CAND" || return 1
    num_check_physics "$omega" "$M19R_PHYSICS" "$NUM_PHYSICS_CAND" || return 1
    before=$(m19r_historical) || return 1
    bin=$NUM_RUN_DIR/test_omega_numeric

    exec 9> /tmp/aien-gb10.lock || m19r_fail "cannot open /tmp/aien-gb10.lock" || return 1
    flock -x 9
    rc=0
    num_build "$omega" "$M19R_PHYSICS" "$bin" "$NUM_RUN_DIR/build.log" || rc=1
    if [ "$rc" = 0 ]; then
        NUM_BINARY_SHA=$(m19r_sha_file "$bin")
        m19r_git "$omega" rev-parse HEAD && NUM_RUN_COMMIT=$M19R_GIT
        NUM_TS=$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ)
        # A failing test exits nonzero; the log still decides, so no m19r_cmd here.
        (cd "$omega" && exec "$bin" 9>&-) > "$NUM_RUN_DIR/gate5.log" 2>&1
        echo "gate binary exit status $?" >> "$NUM_RUN_DIR/build.log"
    fi
    exec 9>&-
    [ "$rc" = 0 ] || return 1

    after=$(m19r_historical) || return 1
    [ "$before" = "$after" ] || m19r_fail "evidence/ changed during the run" || return 1
    m19r_must_candidate "$omega" "$NUM_OMEGA_CAND" || return 1
    m19r_must_candidate "$M19R_PHYSICS" "$NUM_PHYSICS_CAND" || return 1
    num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"
}

num_usage() {
    echo "usage: $M19R_PROG --omega-candidate SHA --physics-candidate SHA [--physics-dir DIR] [--record]"
}

num_main() {
    local physics_dir= rc=0 run_json
    NUM_OMEGA_CAND= NUM_PHYSICS_CAND= NUM_RECORD=0 NUM_PERMANENT= NUM_DIGEST=
    while [ $# -gt 0 ]; do
        case $1 in
            -h|--help) num_usage; exit 0;;
            --omega-candidate|--physics-candidate|--physics-dir)
                [ $# -ge 2 ] || { num_usage >&2; echo "$M19R_PROG: $1 needs a value" >&2; exit 2; }
                case $1 in
                    --omega-candidate) NUM_OMEGA_CAND=$2;;
                    --physics-candidate) NUM_PHYSICS_CAND=$2;;
                    --physics-dir) physics_dir=$2;;
                esac
                shift;;
            --omega-candidate=*) NUM_OMEGA_CAND=${1#*=};;
            --physics-candidate=*) NUM_PHYSICS_CAND=${1#*=};;
            --physics-dir=*) physics_dir=${1#*=};;
            --record) NUM_RECORD=1;;
            *) num_usage >&2; echo "$M19R_PROG: unrecognized argument: $1" >&2; exit 2;;
        esac
        shift
    done
    if [ -z "$NUM_OMEGA_CAND" ] || [ -z "$NUM_PHYSICS_CAND" ]; then
        num_usage >&2; echo "$M19R_PROG: --omega-candidate and --physics-candidate are required" >&2; exit 2
    fi
    M19R_PHYSICS=$(realpath -m "${physics_dir:-$(dirname "$M19R_OMEGA")/physics}")
    m19r_build_canon || { echo "Gate 5 failed: cannot build tools/json_canon.c" >&2; exit 1; }
    trap 'rm -rf "$M19R_TMP"' EXIT
    NUM_RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')
    NUM_RUN_DIR=$M19R_OMEGA/build/qual-runs/gate5-$NUM_RUN_ID
    mkdir -p "$(dirname "$NUM_RUN_DIR")" && mkdir "$NUM_RUN_DIR" || exit 1
    rm -f "$M19R_TMP/error"
    if num_qualify; then
        echo "Gate 5 PASS: receipt digest $NUM_DIGEST"
        [ -z "$NUM_PERMANENT" ] || echo "Permanent receipt: $NUM_PERMANENT"
    else
        rc=1
        [ ! -s "$M19R_TMP/error" ] || M19R_ERR=$(cat "$M19R_TMP/error")
        echo "Gate 5 FAILED: $M19R_ERR" >&2
        echo "No receipt written." >&2
    fi
    run_json="{\"run_id\":$(m19r_jstr "$NUM_RUN_ID"),\"status\":\"$([ "$rc" = 0 ] && echo PASS || echo FAILED)\""
    run_json+=",\"physics_dir\":$(m19r_jstr "$M19R_PHYSICS")"
    [ "$rc" = 0 ] || run_json+=",\"error\":$(m19r_jstr "$M19R_ERR")"
    [ -z "$NUM_DIGEST" ] || run_json+=",\"receipt_digest\":\"$NUM_DIGEST\""
    [ -z "$NUM_PERMANENT" ] || run_json+=",\"permanent_receipt\":$(m19r_jstr "$NUM_PERMANENT")"
    run_json+="}"
    printf '%s' "$run_json" | "$JSON_CANON" --pretty > "$NUM_RUN_DIR/run.json" || rc=1
    echo "Gate 5 run evidence: $NUM_RUN_DIR/run.json"
    exit "$rc"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -u
    num_main "$@"
fi
