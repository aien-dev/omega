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
#   5b. lanes B and C: coder_spec_sha256 = SHA-256 of CODER_SPEC.md; TPS1 profile digest at offset 28; envelope
#      448 / 64 / 1.0e-3 two-sided as in CODER_SPEC section 9; TCR1 range and TCA1 rANS L = 2^23; 56-byte header;
#      crumbs and learner SHA-256 equal the pins in generate_sealed_data.sh; LINEAGE NOTE in profile and prereg;
#      blinding states the temporal layer and the same-uid limit; runtime_digest.sh exists
#   6. profile_digest is the literal "SIDECAR"; outside comments, FILL_AT_FREEZE may appear only in runtime_digest
#   --freeze also: no FILL_AT_FREEZE marker, the .sha256 sidecar matches the file bytes, and runtime_digest equals
#      the output of runtime_digest.sh.
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


# 5b. lanes B and C, integration fields (EXP-001)
for k in coder_spec_path coder_tool coder_header_bits coder_overhead_definition coder_envelope_center_bits \
    coder_envelope_a_bits coder_envelope_b_ub_per_symbol coder_envelope_units coder_envelope_basis \
    probability_stream_format probability_stream_profile_digest_offset probability_stream_model_digest_rule \
    probability_stream_dataset_digest_rule dataset_generator_command dataset_generator_learner_sha256 \
    dataset_generator_lineage_records_differing_seed1 dataset_generator_lineage_records_seed1 \
    blinding_protocol_path blinding_separate_sealed_user runtime_digest_rule candidate_memorizer_decision \
    candidate_large_artifacts; do
    grep -q "^$k = " "$toml" || bad "integration field $k missing"
done
# 5c. calibration protocol field names (docs/turing/protocols calibration protocol lines 155-277), plus the verdict rule
T="$dir/docs/turing/protocols/turing-instrument-calibration-validation-protocol-v1-0.tex"
if [ -f "$T" ]; then
    for k in $(awk 'NR>=155 && NR<=277' "$T" | grep -E '^[a-zA-Z_]+$'); do
        grep -q "^$k = " "$toml" || bad "protocol field $k missing"
    done
else
    bad "calibration protocol $T missing"
fi
grep -q "^verdict_inconclusive_trigger = " "$toml" || bad "verdict_inconclusive_trigger missing"
spec="$dir/$(val coder_spec_path)"
if [ -f "$spec" ]; then
    [ "$(val coder_spec_sha256)" = "$(sha256sum "$spec" | cut -c1-64)" ] || bad "coder_spec_sha256 != SHA-256 of $spec"
    grep -q '^| 28 | 32 | profile_digest |' "$spec" || bad "CODER_SPEC TPS1 profile_digest not at offset 28"
    [ "$(val probability_stream_profile_digest_offset)" = 28 ] || bad "probability_stream_profile_digest_offset != 28"
    grep -q 'overhead - 448 | <= 64 + 1.0e-3 x N' "$spec" || bad "CODER_SPEC envelope differs from 448 / 64 / 1.0e-3"
    grep -q 'Constants: L = 2^23' "$spec" || bad "CODER_SPEC rANS L is not 2^23"
    grep -q 'header (56 bytes)' "$spec" || bad "CODER_SPEC coded header is not 56 bytes"
else
    bad "coder spec $spec missing"
fi
[ "$(val coder_header_bits)" = 448 ] || bad "coder_header_bits != 448 (56 bytes)"
[ "$(val coder_envelope_center_bits)" = "$(val coder_header_bits)" ] || bad "envelope center != header bits"
[ "$(val coder_envelope_a_bits)" = 64 ] || bad "coder_envelope_a_bits != 64"
[ "$(val coder_envelope_b_ub_per_symbol)" = 1000 ] || bad "coder_envelope_b_ub_per_symbol != 1000 (1.0e-3 bits)"
case "$(val coder_envelope)" in TWO-SIDED*) ;; *) bad "coder_envelope must be TWO-SIDED" ;; esac
case "$(val reference_coder_1)" in "TCR1 v1 range"*) ;; *) bad "reference_coder_1 is not TCR1 v1 range" ;; esac
case "$(val reference_coder_2)" in "TCA1 v1 rANS"*"L = 2^23"*) ;; *) bad "reference_coder_2 is not TCA1 v1 rANS, L = 2^23" ;; esac
gen="$dir/calibration/scripts/generate_sealed_data.sh"
pin() { grep "^$1=" "$gen" | head -1 | grep -o '[0-9a-f]\{64\}'; }
if [ -f "$gen" ]; then
    [ "$(val dataset_generator_crumbs_sha256)" = "$(pin CRUMBS_SHA256)" ] || bad "crumbs SHA-256 differs from generate_sealed_data.sh"
    [ "$(val dataset_generator_learner_sha256)" = "$(pin LEARNER_SHA256)" ] || bad "learner SHA-256 differs from generate_sealed_data.sh"
    grep -q 'sealed_seeds_per_group' "$gen" || bad "generate_sealed_data.sh does not read sealed_seeds_per_group"
else
    bad "$gen missing"
fi
[ "$(val dataset_generator_command)" = "crumbs experiment --learner L --seed S --out DIR" ] || bad "dataset_generator_command"
case "$(val dataset_generator)" in *"LINEAGE NOTE"*) ;; *) bad "dataset_generator lacks the LINEAGE NOTE" ;; esac
grep -q 'LINEAGE NOTE' "$dir/calibration/preregistration/EXP-001.md" || bad "prereg lacks the LINEAGE NOTE"
[ -f "$dir/$(val blinding_protocol_path)" ] || bad "blinding protocol missing"
[ "$(val blinding_separate_sealed_user)" = no ] || bad "blinding_separate_sealed_user must be no"
case "$(val blinding_method)" in *"temporal"*"same user id"*) ;; *) bad "blinding_method must state the temporal layer and the same-uid limit" ;; esac
[ -f "$dir/calibration/scripts/runtime_digest.sh" ] || bad "runtime_digest.sh missing"

# 6. freeze mode
nfill=$(grep -v "^[[:space:]]*#" "$toml" | grep -c "FILL_AT_FREEZE" || true)
other=$(grep -v "^[[:space:]]*#" "$toml" | grep "FILL_AT_FREEZE" | grep -v "^runtime_digest = " || true)
[ -z "$other" ] || bad "FILL_AT_FREEZE outside runtime_digest: $(printf "%s" "$other" | cut -c1-60)"
if [ "$freeze" = 1 ]; then
    [ "$nfill" = 0 ] || bad "$nfill FILL_AT_FREEZE markers remain"
    [ -f "$side" ] || bad "sidecar $side missing"
    if [ -f "$side" ]; then
        want=$(cut -c1-64 "$side"); got=$(sha256sum "$toml" | cut -c1-64)
        [ "$want" = "$got" ] || bad "sidecar $want != sha256 of profile bytes $got"
    fi
    rd=$(sh "$dir/calibration/scripts/runtime_digest.sh" 2>&1) || bad "runtime_digest.sh: $rd"
    [ "$(val runtime_digest)" = "$rd" ] || bad "runtime_digest != runtime_digest.sh output $rd"
fi

if [ "$fails" -ne 0 ]; then
    echo "check_profile: $fails failure(s)"
    exit 1
fi
echo "check_profile: OK ($nreq required fields + L(M) fields present, constants match code, $nfill FILL_AT_FREEZE markers$([ "$freeze" = 1 ] && echo ', freeze mode, sidecar matches'))"
