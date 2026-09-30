# tc_exp.sh: experiment selection for the calibration scripts (POSIX sh; sourced, never run).
#
#   EXP_ID=EXP-001   (default) Turing-profile-v1.0, calibration/experiments/EXP-001, preregistration EXP-001.md
#   EXP_ID=EXP-001R  successor run: Turing-profile-v1.1, calibration/experiments/EXP-001R, preregistration EXP-001R.md
#
#   EXP_DIR EXP_PREREG EXP_PROFILE_VER EXP_PROFILE_ID EXP_PROFILE EXP_SIDECAR EXP_PROFILE_NAME
#   EXP_CAND_STORE   committed candidate files. EXP-001R uses the EXP-001 files unchanged (same seven
#                    candidates, no refit), so both experiments point at calibration/experiments/EXP-001/candidates.
#   EXP_POWER        the power simulation output; the design is unchanged, so always the EXP-001 file.
EXP_ID="${EXP_ID:-EXP-001}"
case "$EXP_ID" in
EXP-001) EXP_PROFILE_VER=1.0 ;;
EXP-001R) EXP_PROFILE_VER=1.1 ;;
*) echo "tc_exp: unknown EXP_ID '$EXP_ID' (EXP-001 or EXP-001R)" >&2; exit 2 ;;
esac
EXP_DIR="calibration/experiments/$EXP_ID"
EXP_PREREG="calibration/preregistration/$EXP_ID.md"
EXP_PROFILE_ID="Turing-profile-v$EXP_PROFILE_VER"
EXP_PROFILE_NAME="$EXP_PROFILE_ID.toml"
EXP_PROFILE="calibration/profiles/$EXP_PROFILE_NAME"
EXP_SIDECAR="calibration/profiles/$EXP_PROFILE_ID.sha256"
EXP_CAND_STORE="calibration/experiments/EXP-001/candidates"
EXP_POWER="calibration/experiments/EXP-001/power_simulation_output.txt"
# EXP-001R only: burned seeds (the six EXP-001 sealed seeds, one decimal per line), the amended overlap rule G6,
# and the EXP-001 sealed root whose traces and ledgers join the dev/burned comparison list.
EXP_BURNED_FILE="" EXP_G6_RULE=strict EXP_BURNED_SEALED=""

if [ "$EXP_ID" = EXP-001R ]; then
    EXP_BURNED_FILE="calibration/scripts/burned_seeds_exp001.txt"
    EXP_G6_RULE=amended
    EXP_BURNED_SEALED="${TC_EXP001_SEALED:-${HOME:-}/aien-data/turing-cal/sealed/d3cba292b9282116d1e374db22344bca4d47717e}"
fi
export EXP_ID EXP_BURNED_FILE EXP_G6_RULE EXP_BURNED_SEALED
