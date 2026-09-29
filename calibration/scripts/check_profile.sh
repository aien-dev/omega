#!/bin/sh
# check_profile.sh: Turing-profile-v1.0.toml must agree with the code it describes.
#
#   TXA_CAND=build/turing-exp001-a/turing-cal-candidates sh calibration/scripts/check_profile.sh [--freeze]
#
# Checks (any failure exits 1):
#   1. format: every non-comment, non-blank line is `key = value`; keys unique; no em or en dashes
#   2. every field the brief requires is present (46 names)
#   3. values equal the C constants printed by `turing-cal-candidates consts` (compiled from
#      src/turing/ty_math.h, ty_model.h, ty_ctr1.h): K, qbits, 65536, floor, header bits, version,
#      digest domain, micro-bit unit, error bound, CTR1 record size, depth clip, SUBMIT op value,
#      masks and model digests of B2 / M_candidate
#   4. L(M) fields equal 232 + rows x (keybits + 128) for B3 (16 rows, 4 key bits)
#   5. optional cross-checks when the files exist: candidates printout (TXA_CANDIDATES) L(M) values,
#      power simulation output (CHOSEN_SEEDS_PER_GROUP == sealed_seeds_per_group)
#   6. profile_digest is the literal "SIDECAR"
#   --freeze also: no FILL_AT_FREEZE marker anywhere, and the .sha256 sidecar matches the file bytes.
# No Python. POSIX sh + sed + grep + sha256sum.
set -eu
dir=$(cd "$(dirname "$0")/../.." && pwd)
toml="$dir/calibration/profiles/Turing-profile-v1.0.toml"
side="$dir/calibration/profiles/Turing-profile-v1.0.sha256"
cand="${TXA_CAND:-$dir/build/turing-exp001-a/turing-cal-candidates}"
cands="${TXA_CANDIDATES:-$dir/calibration/experiments/EXP-001/candidates_dev.txt}"
power="${TXA_POWER_OUT:-$dir/calibration/experiments/EXP-001/power_simulation_output.txt}"
freeze=0
[ "${1:-}" = "--freeze" ] && freeze=1
fails=0
bad() { echo "check_profile: FAIL: $*"; fails=$((fails + 1)); }

[ -f "$toml" ] || { echo "check_profile: FAIL: $toml missing"; exit 1; }
[ -x "$cand" ] || { echo "check_profile: FAIL: $cand not built (make turing-exp001-a-build)"; exit 1; }

# 1. format
nonkv=$(grep -v '^[[:space:]]*#' "$toml" | grep -v '^[[:space:]]*$' | grep -cv '^[a-z][a-z0-9_A-Z]* = ' || true)
[ "$nonkv" = 0 ] || bad "$nonkv lines are not flat 'key = value'"
dups=$(sed -n 's/^\([a-z][a-z0-9_A-Z]*\) = .*/\1/p' "$toml" | sort | uniq -d)
[ -z "$dups" ] || bad "duplicate keys: $dups"
if grep -q "$(printf '\342\200\224')\|$(printf '\342\200\223')" "$toml"; then bad "em or en dash in profile"; fi

val() { sed -n "s/^$1 = //p" "$toml" | head -1 | sed 's/^"\(.*\)"$/\1/'; }

# 2. required fields (brief, PROMPT 0)
for k in profile_id profile_version profile_digest task_definition baseline_family baseline_encoding \
    candidate_encoding_rules shared_background_definition model_description_language model_serialization \
    model_cost_method dataset_generator development_split calibration_split sealed_test_split \
    test_generation_seed_commitment holdout_commitment blinding_method probability_representation \
    probability_quantization zero_probability_policy normalization_policy ideal_codelength_method \
    reference_coder_1 reference_coder_2 coder_termination_convention coder_header_accounting \
    coder_metadata_accounting uncertainty_method correlation_handling block_resampling_policy \
    number_of_replicates number_of_trajectories trajectory_length stopping_rule energy_instrumentation \
    idle_baseline_method energy_attribution_method software_versions compiler_versions runtime_digest \
    hardware_identity independent_verification_requirements baseline_sensitivity_plan \
    model_code_sensitivity_plan overlap_audit failure_reporting_policy \
    lm_free_background lm_free_constants lm_charged_constants lm_learned_parameters lm_parameter_precision \
    lm_structural_metadata lm_dependencies lm_lookup_tables lm_embedded_datasets lm_generated_code \
    lm_external_artifacts lm_model_headers lm_version_info sealed_seeds_per_group sealed_groups; do
    grep -q "^$k = " "$toml" || bad "required field $k missing"
done
nreq=46

# 3. constants
consts=$("$cand" consts)
c() { printf '%s\n' "$consts" | sed -n "s/^$1=//p"; }
eq() { # toml_key const_name
    tv=$(val "$1"); cv=$(c "$2")
    [ -n "$cv" ] || { bad "no constant $2"; return; }
    [ "$tv" = "$cv" ] || bad "$1 = '$tv' but code has $2 = '$cv'"
}
eq alphabet_size_K K
eq probability_qbits qbits
eq probability_total qone
eq probability_floor floor_q
eq model_header_bits model_header_bits
eq model_code_version model_version
eq model_digest_domain model_domain
eq ub_per_bit ub_per_bit
eq qerr_milli_ub_per_symbol qerr_milli_ub_per_symbol
eq ctr1_record_bytes ctr1_record_bytes
eq depth_clip depth_clip
eq op_submit_value op_submit
eq baseline_b2_mask b2_mask
eq candidate_m_candidate_mask m_candidate_mask
eq candidate_m_mem_mask m_mem_mask
eq candidate_b3_mask b3_mask
eq baseline_b2_model_digest b2_digest
eq candidate_m_candidate_model_digest m_candidate_digest
[ "$(val profile_digest)" = "SIDECAR" ] || bad "profile_digest must be the literal SIDECAR"
[ "$(val profile_id)" = "Turing-profile-v1.0" ] || bad "profile_id"

# 4. L(M) formula for B3: 104 + 16(K-1) + 16 rows x (keybits + 16(K-1))
K=$(c K); hb=$(c model_header_bits); qb=$(c qbits); kb=$(c b3_keybits)
b3=$((hb + qb * (K - 1) + 16 * (kb + qb * (K - 1))))
[ "$(val candidate_b3_lm_bits)" = "$b3" ] || bad "candidate_b3_lm_bits != $b3"
if ! grep -q "^lm_formula = \"L(M) = $hb + 16(K-1)" "$toml"; then bad "lm_formula header term != $hb"; fi

# 5. optional cross-checks
if [ -f "$cands" ]; then
    lm() { sed -n "s/^$1 .* lm_bits=\([0-9]*\) .*/\1/p" "$cands"; }
    [ "$(lm B2_order1)" = "$(val baseline_b2_lm_bits)" ] || bad "B2 L(M) differs from $cands"
    [ "$(lm M_candidate)" = "$(val candidate_m_candidate_lm_bits)" ] || bad "M_candidate L(M) differs"
    [ "$(lm M_mem)" = "$(val candidate_m_mem_lm_bits)" ] || bad "M_mem L(M) differs"
    [ "$(lm M_mem_seed1)" = "$(val candidate_m_mem_seed1_lm_bits)" ] || bad "M_mem_seed1 L(M) differs"
    [ "$(lm B3_heuristic)" = "$(val candidate_b3_lm_bits)" ] || bad "B3 L(M) differs"
else
    echo "check_profile: note: $cands absent, candidate L(M) cross-check skipped"
fi
if [ -f "$power" ]; then
    pn=$(sed -n 's/^CHOSEN_SEEDS_PER_GROUP=\([0-9]*\).*/\1/p' "$power")
    [ "$pn" = "$(val sealed_seeds_per_group)" ] || bad "sealed_seeds_per_group != power simulation choice ($pn)"
else
    echo "check_profile: note: $power absent, sample-size cross-check skipped"
fi

# 6. freeze mode
nfill=$(grep -c 'FILL_AT_FREEZE' "$toml" || true)
if [ "$freeze" = 1 ]; then
    [ "$nfill" = 0 ] || bad "$nfill FILL_AT_FREEZE markers remain"
    [ -f "$side" ] || bad "sidecar $side missing"
    if [ -f "$side" ]; then
        want=$(cut -c1-64 "$side"); got=$(sha256sum "$toml" | cut -c1-64)
        [ "$want" = "$got" ] || bad "sidecar $want != sha256 of profile bytes $got"
    fi
fi

if [ "$fails" -ne 0 ]; then
    echo "check_profile: $fails failure(s)"
    exit 1
fi
echo "check_profile: OK ($nreq required fields + L(M) fields present, constants match code, $nfill FILL_AT_FREEZE markers$([ "$freeze" = 1 ] && echo ', freeze mode, sidecar matches'))"
