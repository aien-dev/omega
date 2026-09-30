#!/bin/bash
# gate14_combine.sh -- M19R Combined Foundation Admission Gate (Gate 14).
#
#   tools/gate14_combine.sh [--omega-candidate SHA] [--physics-candidate SHA]
#                           [--evidence-dir DIR] RECEIPT...
#
# Read-only aggregation of the foundation leg receipts
# (docs/m19r-recovery-program.md in aien-architecture, "Combined Foundation
# Admission Gate"). Each RECEIPT is one leg, recognised by its schema:
#
#   AIEN_M19R_QUALIFICATION_V1  Gates 1/2  tools/m19r_qualify.sh (receipt-preview.json
#                                          or evidence/M19R/<digest>.json)
#   AIEN_M19R_FORGE_GATES_V1    Gates 3/4  physics tests/run_forge_gates.sh
#   AIEN_OMEGA_NUMERIC_0_V1     Gate 5     tests/run_numeric_gates.sh
#
# Accepts only when every one of the eight Gate 14 criteria is backed by a
# PASS leg and, for every leg: the file is valid JSON; receipt_digest is the
# SHA-256 of the canonical body without it (and equals the file name when
# the file is named <64-hex>.json); candidate_git_commit (omega) and
# physics_candidate_git_commit are full 40-hex ids, identical across ALL legs
# (and equal to --omega-candidate / --physics-candidate when given);
# candidate_trees_clean.omega and .physics are both true. Any
# hardware_descriptor_digest present must agree across legs. A schema seen
# twice or not known is refused.
#
# On accept prints the combined receipt digest, and with --evidence-dir
# writes <dir>/<receipt_digest>.json (pretty, created exclusively, mode 0444;
# the spec location is evidence/GATE14-FOUNDATION/). On refuse prints
# "REFUSED: <reason>" on stderr and writes nothing.
# Exit 0 accept, 1 refuse, 2 bad arguments.
#
# Digests use tools/json_canon.c (the same canonical form as the legs). They
# make alteration detectable; they are not signatures. Shell + coreutils +
# jq + that C helper. No Python.

G14_OMEGA=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
G14_SCHEMA=AIEN_M19R_GATE14_FOUNDATION_V1
G14_ERR=

g14_refuse() { G14_ERR=$1; return 1; }

g14_build_canon() {
    G14_TMP=$(mktemp -d "${TMPDIR:-/tmp}/gate14.XXXXXX") || return 1
    JSON_CANON=$G14_TMP/json_canon
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$G14_OMEGA/src" -o "$JSON_CANON" \
        "$G14_OMEGA/tools/json_canon.c" "$G14_OMEGA/src/sha256.c" -lm
}

# g14_criteria SCHEMA -- the Gate 14 criteria one leg backs.
g14_criteria() {
    case $1 in
        AIEN_M19R_QUALIFICATION_V1)
            echo EVIDENCE_IMMUTABLE RECEIPTS_OBSERVED_NOT_ASSERTED DEVICE_MEMORY_LIFECYCLE COMPLETION_EXACTLY_ONCE LONG_RUNNING_GPU_SOAK;;
        AIEN_M19R_FORGE_GATES_V1) echo FORGE_BOUNDARY HARDWARE_ID_OBSERVED;;
        AIEN_OMEGA_NUMERIC_0_V1) echo FP32_CPU_GB10_PARITY;;
        *) return 1;;
    esac
}
G14_ALL_CRITERIA="EVIDENCE_IMMUTABLE RECEIPTS_OBSERVED_NOT_ASSERTED DEVICE_MEMORY_LIFECYCLE COMPLETION_EXACTLY_ONCE LONG_RUNNING_GPU_SOAK FORGE_BOUNDARY HARDWARE_ID_OBSERVED FP32_CPU_GB10_PARITY"

# g14_pass_rule SCHEMA -- jq expression, true when the leg receipt records a PASS.
# results (every leg but Gate 1/2): a non-empty test_results list, every
# entry PASS, its length equal to observed_test_count and observed_pass_count.
# Gate 1/2 also needs the soak thresholds m19r_qualify.sh enforces (>= 100000
# cycles, bytes churned > 2x physical memory) and exactly 18 OMEGA_ACCEL_RESIDENT_
# results. The Gate 5 leg must have run on its candidates (run_git_commit,
# physics_lock).
# The Gate 1/2 receipt has no status field (it is written only on success),
# so its PASS is read from its observed counts, results and soak.
G14_RESULTS_RULE='((.test_results | type) == "array"
            and (.test_results | length) > 0
            and (.test_results | length) == .observed_test_count
            and .observed_pass_count == .observed_test_count
            and all(.test_results[]; .status == "PASS"))'
g14_pass_rule() {
    case $1 in
        AIEN_M19R_QUALIFICATION_V1) echo '
            (has("status") | not)
            and .observed_fail_count == 0
            and (.observed_gate_results_count | type) == "number"
            and .observed_gate_results_count > 0
            and .observed_pass_count == .observed_gate_results_count
            and (.test_results | type) == "array"
            and (.test_results | length) == .observed_gate_results_count
            and all(.test_results[]; .status == "PASS")
            and .soak.passed == true
            and (.soak.cycles | type) == "number" and .soak.cycles >= 100000
            and (.soak.physical_memory_bytes | type) == "number" and .soak.physical_memory_bytes > 0
            and (.soak.bytes_churned | type) == "number"
            and .soak.bytes_churned > 2 * .soak.physical_memory_bytes
            and ([.test_results[] | select((.id | type) == "string" and (.id | startswith("OMEGA_ACCEL_RESIDENT_"))) | .id] | unique | length) == 18
            and ([.test_results[] | select((.id | type) == "string" and (.id | startswith("OMEGA_ACCEL_RESIDENT_")))] | length) == 18
            and (.m19_observations | type) == "object"
            and .m19_observations.m19_gates_completed == 18
            and .m19_observations.m19_gates_passed == 18
            and (.m19_observations.regression_gates_completed | type) == "number"
            and .m19_observations.regression_gates_completed > 0
            and .m19_observations.regression_gates_passed == .m19_observations.regression_gates_completed';;
        AIEN_M19R_FORGE_GATES_V1) echo '
            .status == "PASS"
            and .observed_fail_count == 0
            and .gates.GATE_3_FORGE_0.status == "PASS"
            and .gates.GATE_3_FORGE_0.gate_binary_exit_status == 0
            and .gates.GATE_4_FORGE_HWID.status == "PASS"
            and .gates.GATE_4_FORGE_HWID.gate_binary_exit_status == 0
            and ([.gates[].candidate_binary_sha256] | all(type == "string" and test("^[0-9a-f]{64}$")))
            and .gates.GATE_4_FORGE_HWID.hardware_descriptor_digest == .hardware_descriptor_digest
            and (.hardware_descriptor_digest | type) == "string"
            and results';;
        AIEN_OMEGA_NUMERIC_0_V1) echo '
            .status == "PASS"
            and .gate_binary_exit_status == 0
            and .observed_fail_count == 0
            and .run_git_commit == .candidate_git_commit
            and .physics_lock == .physics_candidate_git_commit
            and (.hardware_descriptor_digest | type) == "string"
            and results';;
    esac
}

# g14_check_leg FILE -- validate one leg. Sets G14_LEG_SCHEMA, G14_LEG_DIGEST,
# G14_LEG_OMEGA, G14_LEG_PHYSICS, G14_LEG_DESC.
g14_check_leg() {
    local f=$1 name recomputed rule s
    [ -f "$f" ] && [ -r "$f" ] || g14_refuse "cannot read receipt $f" || return 1
    # every check reads one private copy, so the file cannot change between checks
    s=$(mktemp "$G14_TMP/leg.XXXXXX") && cp -- "$f" "$s" || g14_refuse "cannot copy receipt $f" || return 1
    "$JSON_CANON" --check < "$s" || g14_refuse "$f is not valid JSON" || return 1
    jq -e 'type == "object"' "$s" > /dev/null || g14_refuse "$f is not a JSON object" || return 1
    G14_LEG_SCHEMA=$(jq -r '.schema // ""' "$s")
    g14_criteria "$G14_LEG_SCHEMA" > /dev/null || g14_refuse "$f has unknown schema '$G14_LEG_SCHEMA'" || return 1
    G14_LEG_DIGEST=$(jq -r '.receipt_digest // ""' "$s")
    [[ $G14_LEG_DIGEST =~ ^[0-9a-f]{64}$ ]] || g14_refuse "$f has no 64-hex receipt_digest" || return 1
    recomputed=$(jq -c 'del(.receipt_digest)' "$s" | "$JSON_CANON" --sha256) ||
        g14_refuse "cannot digest $f" || return 1
    [ "$recomputed" = "$G14_LEG_DIGEST" ] || g14_refuse "$f receipt_digest does not match its content" || return 1
    name=$(basename "$f")
    if [[ $name =~ ^[0-9a-fA-F]{64}\.json$ ]]; then
        [ "${name%.json}" = "$G14_LEG_DIGEST" ] || g14_refuse "$f is named for a different digest" || return 1
    fi
    G14_LEG_OMEGA=$(jq -r '.candidate_git_commit // ""' "$s")
    G14_LEG_PHYSICS=$(jq -r '.physics_candidate_git_commit // ""' "$s")
    [[ $G14_LEG_OMEGA =~ ^[0-9a-f]{40}$ ]] || g14_refuse "$f candidate_git_commit is not a full 40-hex id" || return 1
    [[ $G14_LEG_PHYSICS =~ ^[0-9a-f]{40}$ ]] || g14_refuse "$f physics_candidate_git_commit is not a full 40-hex id" || return 1
    jq -e '.candidate_trees_clean.omega == true and .candidate_trees_clean.physics == true' "$s" > /dev/null ||
        g14_refuse "$f was not made from clean omega and physics trees" || return 1
    rule=$(g14_pass_rule "$G14_LEG_SCHEMA")
    jq -e "def results: $G14_RESULTS_RULE; $rule" "$s" > /dev/null 2>&1 || g14_refuse "$f ($G14_LEG_SCHEMA) does not record a PASS" || return 1
    G14_LEG_DESC=$(jq -r '.hardware_descriptor_digest // ""' "$s")
    [ -z "$G14_LEG_DESC" ] || { [[ $G14_LEG_DESC =~ ^[0-9a-f]{64}$ ]] && [[ ! $G14_LEG_DESC =~ ^0+$ ]]; } ||
        g14_refuse "$f hardware_descriptor_digest is not 64 hex (or is all zeros)" || return 1
}

# g14_combine OMEGA_EXPECTED PHYSICS_EXPECTED TS RECEIPT... -- check every leg
# and build the combined receipt. Sets G14_DIGEST and G14_RECEIPT.
g14_combine() {
    local want_o=${1,,} want_p=${2,,} ts=$3 f omega= physics= desc= seen= c crit legs= criteria= body
    shift 3
    [ $# -gt 0 ] || g14_refuse "no receipts given" || return 1
    declare -A by_crit=()
    for f in "$@"; do
        g14_check_leg "$f" || return 1
        case " $seen " in *" $G14_LEG_SCHEMA "*) g14_refuse "two receipts for $G14_LEG_SCHEMA" || return 1;; esac
        seen="$seen $G14_LEG_SCHEMA"
        if [ -z "$omega" ]; then
            omega=$G14_LEG_OMEGA physics=$G14_LEG_PHYSICS
        fi
        [ "$G14_LEG_OMEGA" = "$omega" ] ||
            g14_refuse "omega commit mismatch: $f has $G14_LEG_OMEGA, first leg has $omega" || return 1
        [ "$G14_LEG_PHYSICS" = "$physics" ] ||
            g14_refuse "physics commit mismatch: $f has $G14_LEG_PHYSICS, first leg has $physics" || return 1
        if [ -n "$G14_LEG_DESC" ]; then
            [ -z "$desc" ] || [ "$desc" = "$G14_LEG_DESC" ] ||
                g14_refuse "hardware_descriptor_digest differs between legs" || return 1
            desc=$G14_LEG_DESC
        fi
        for c in $(g14_criteria "$G14_LEG_SCHEMA"); do by_crit[$c]=$G14_LEG_DIGEST; done
        legs+="${legs:+,}{\"schema\":\"$G14_LEG_SCHEMA\",\"receipt_digest\":\"$G14_LEG_DIGEST\"}"
    done
    [ -z "$want_o" ] || [ "$want_o" = "$omega" ] ||
        g14_refuse "legs are for omega $omega, not the expected $want_o" || return 1
    [ -z "$want_p" ] || [ "$want_p" = "$physics" ] ||
        g14_refuse "legs are for physics $physics, not the expected $want_p" || return 1
    for crit in $G14_ALL_CRITERIA; do
        [ -n "${by_crit[$crit]:-}" ] || g14_refuse "missing leg: no receipt backs $crit" || return 1
        criteria+="${criteria:+,}\"$crit\":{\"status\":\"PASS\",\"receipt_digest\":\"${by_crit[$crit]}\"}"
    done
    body="{\"schema\":\"$G14_SCHEMA\",\"gate\":\"M19R_GATE14_COMBINED_FOUNDATION_ADMISSION\",\"status\":\"PASS\""
    body+=",\"timestamp_utc\":\"$ts\""
    body+=",\"candidate_git_commit\":\"$omega\",\"physics_candidate_git_commit\":\"$physics\""
    body+=",\"candidate_trees_clean\":{\"omega\":true,\"physics\":true}"
    body+=",\"constituent_receipts\":[$legs],\"criteria\":{$criteria}"
    [ -z "$desc" ] || body+=",\"hardware_descriptor_digest\":\"$desc\""
    body+=",\"digest_meaning\":\"integrity only, not authenticity: re-hash the constituent receipts named here\"}"
    G14_DIGEST=$(printf '%s' "$body" | "$JSON_CANON" --sha256) || g14_refuse "combined body is not valid JSON" || return 1
    G14_RECEIPT="{\"receipt_digest\":\"$G14_DIGEST\",${body#\{}"
}

# g14_write DIR -- write G14_RECEIPT to DIR/<digest>.json (pretty, exclusive, 0444).
g14_write() {
    local dir=$1 pretty
    mkdir -p "$dir" || g14_refuse "cannot create $dir" || return 1
    pretty=$G14_TMP/combined.pretty
    printf '%s' "$G14_RECEIPT" | "$JSON_CANON" --pretty > "$pretty" || g14_refuse "cannot format receipt" || return 1
    "$JSON_CANON" --write-exclusive "$dir/$G14_DIGEST.json" < "$pretty" 2> "$pretty.err" ||
        g14_refuse "cannot write $dir/$G14_DIGEST.json: $(cat "$pretty.err")" || return 1
    chmod 0444 "$dir/$G14_DIGEST.json" || g14_refuse "cannot chmod $dir/$G14_DIGEST.json"
}

g14_usage() {
    echo "usage: $0 [--omega-candidate SHA] [--physics-candidate SHA] [--evidence-dir DIR] RECEIPT..." >&2
    exit 2
}

g14_main() {
    local want_o= want_p= ev= receipts=() ts
    while [ $# -gt 0 ]; do
        case $1 in
            --omega-candidate) [ $# -ge 2 ] || g14_usage; want_o=$2; shift 2;;
            --physics-candidate) [ $# -ge 2 ] || g14_usage; want_p=$2; shift 2;;
            --evidence-dir) [ $# -ge 2 ] || g14_usage; ev=$2; shift 2;;
            --) shift; receipts+=("$@"); break;;
            -*) g14_usage;;
            *) receipts+=("$1"); shift;;
        esac
    done
    [ ${#receipts[@]} -gt 0 ] || g14_usage
    for c in "$want_o" "$want_p"; do
        [ -z "$c" ] || [[ $c =~ ^[0-9a-fA-F]{40}$ ]] || { echo "candidates must be full 40-hex SHAs" >&2; exit 2; }
    done
    g14_build_canon || { echo "REFUSED: cannot build tools/json_canon.c" >&2; exit 1; }
    trap 'rm -rf "$G14_TMP"' EXIT
    ts=$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ)
    g14_combine "$want_o" "$want_p" "$ts" "${receipts[@]}" || { echo "REFUSED: $G14_ERR" >&2; exit 1; }
    if [ -n "$ev" ]; then
        g14_write "$ev" || { echo "REFUSED: $G14_ERR" >&2; exit 1; }
        echo "Gate 14 PASS: receipt $ev/$G14_DIGEST.json"
    else
        echo "Gate 14 PASS: receipt digest $G14_DIGEST (not written; no --evidence-dir)"
    fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    g14_main "$@"
fi
