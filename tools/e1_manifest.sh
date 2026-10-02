#!/bin/bash
# e1_manifest.sh -- E1 candidate-to-main artifact equivalence manifest.
#
#   tools/e1_manifest.sh --candidate SHA --main SHA [--physics-dir DIR] [--out FILE]
#
# The E1 chip campaign ran on one commit (the candidate); the squash merge is
# another (main). This tool does not claim the chip ran on main. It records,
# for every E1 artifact, the SHA-256 of the file's content at both commits
# (ABSENT when a commit lacks it), and whether they are equal. With
# --physics-dir it also rebuilds the four E1 chip binaries from the main tree
# with exactly the gcc lines of the gate scripts (named per entry) and records
# their SHA-256, so a reviewer can compare them with the binary digests the
# chip receipts carry. Read-only on git; writes only the manifest.
#
# Artifact classes:
#   chip   compiled into a chip binary or run as a chip gate script; must be equal
#   host   host-side build system or host tests; may differ only if the host
#          tier was rerun on main (tools/e1_combine.sh enforces this)
#
# Shell + git + coreutils + jq + gcc. No Python.
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
CAND= MAIN= PHYS= OUT=
while [ $# -gt 0 ]; do
    case $1 in
        --candidate) CAND=$2; shift 2;;
        --main) MAIN=$2; shift 2;;
        --physics-dir) PHYS=$2; shift 2;;
        --out) OUT=$2; shift 2;;
        *) echo "usage: $0 --candidate SHA --main SHA [--physics-dir DIR] [--out FILE]" >&2; exit 2;;
    esac
done
[[ $CAND =~ ^[0-9a-f]{40}$ && $MAIN =~ ^[0-9a-f]{40}$ ]] || { echo "candidates must be full 40-hex SHAs" >&2; exit 2; }
cd "$HERE" || exit 2
git cat-file -e "$CAND^{commit}" 2>/dev/null || { echo "unknown commit $CAND" >&2; exit 2; }
git cat-file -e "$MAIN^{commit}" 2>/dev/null || { echo "unknown commit $MAIN" >&2; exit 2; }

# Chip-class patterns: sources of the four chip binaries (Gate 5, reduce, ldst,
# transc), the chip gate scripts and manifests, the physics pin. Host-class:
# the Makefile and host-only tests the host tier runs.
CHIP_RE='^(src/omega_numeric.*|src/omega_blackwell_(codegen|encoder|matmul|qmd)\.[ch]|src/forge_realization\.[ch]|src/aegis_verification\.[ch]|src/sha256\.[ch]|src/omega_program_fp32\.[ch]|src/omega_unwritten_trap.*|tests/test_omega_numeric.*|tests/test_omega_reduce\.c|tests/test_omega_ldst_gb10\.c|tests/test_omega_divsqrt_gb10\.c|tests/numeric_oracle\.h|tests/tensor_reduce_multi_tests\.inc|tests/numeric_native/.*|tests/run_numeric_gates\.sh|tests/run_reduce_chip\.sh|tests/chip_run_transc_receipt_allowlist\.txt|tools/chip_run\.sh|tools/run_numeric_ldst_chip\.sh|tools/run_numeric_transc_gate\.sh|tools/run_divsqrt_gate\.sh|tools/run_unwritten_trap\.sh|tools/ldst_nvdisasm_check\.sh|tools/divsqrt_nvdisasm_check\.sh|tools/manifests/.*|physics\.lock)$'
HOST_RE='^(Makefile|mk/numeric-.*\.mk|mk/program-fp32\.mk|tests/test_omega_transc\.c|tests/test_numeric_lifecycle\.c|tests/test_omega_unwritten_trap\.c|tests/test_chip_run.*\.sh|tools/numeric_check_sweep\.sh|tools/divsqrt_check_sweep\.sh)$'

sha_at() { git cat-file -e "$1:$2" 2>/dev/null && git show "$1:$2" | sha256sum | cut -c1-64 || echo ABSENT; }

files=$( (git ls-tree -r --name-only "$CAND"; git ls-tree -r --name-only "$MAIN") | sort -u | grep -E "$CHIP_RE|$HOST_RE")
entries= n_eq=0 n_ne=0 chip_ne=0
for f in $files; do
    a=$(sha_at "$CAND" "$f"); b=$(sha_at "$MAIN" "$f")
    if echo "$f" | grep -Eq "$CHIP_RE"; then cls=chip; else cls=host; fi
    if [ "$a" = "$b" ]; then eq=true; n_eq=$((n_eq+1)); else eq=false; n_ne=$((n_ne+1)); [ $cls = chip ] && chip_ne=$((chip_ne+1)); fi
    entries+="${entries:+,}{\"path\":\"$f\",\"class\":\"$cls\",\"sha256_candidate\":\"$a\",\"sha256_main\":\"$b\",\"equal\":$eq}"
done

# Rebuild the four chip binaries from the main tree (gcc lines copied from the
# gate scripts; the source list of each is what the chip ran).
bins=
if [ -n "$PHYS" ]; then
    [ -d "$PHYS/nvrm" ] || { echo "no physics tree at $PHYS" >&2; exit 2; }
    [ "$(git rev-parse HEAD)" = "$MAIN" ] || { echo "checkout must be at --main $MAIN to rebuild" >&2; exit 2; }
    [ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "tracked files are modified; refusing to rebuild" >&2; exit 2; }
    NV=$PHYS/third_party/nvidia-open-580.173.02
    INC=(-Isrc -I"$PHYS/nvrm" -I"$PHYS/m16" -I"$NV/src/common/sdk/nvidia/inc" -I"$NV/kernel-open/common/inc" -I"$NV/kernel-open/nvidia-uvm" -I"$NV/src/nvidia/arch/nvalloc/unix/include")
    FORGE="src/forge_realization.c src/aegis_verification.c src/sha256.c $PHYS/forge/forge_descriptor.c $PHYS/forge/forge_realize.c $PHYS/sha256_clean.c $PHYS/nvrm/nvrm.c $PHYS/m16/m16_native.c"
    BLK="src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c"
    TMPB=$(mktemp -d "${TMPDIR:-/tmp}/e1manifest.XXXXXX") || exit 2
    trap 'rm -rf "$TMPB"' EXIT
    build() { # NAME SOURCE_SCRIPT FLAGS... -- SOURCES...
        local name=$1 script=$2; shift 2; local flags=() s
        while [ "$1" != "--" ]; do flags+=("$1"); shift; done; shift
        if gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off "${flags[@]}" "${INC[@]}" -o "$TMPB/$name" "$@" > "$TMPB/$name.log" 2>&1; then
            s=$(sha256sum "$TMPB/$name" | cut -c1-64)
        else s=BUILD_FAILED; fi
        bins+="${bins:+,}{\"gate\":\"$name\",\"build_line_of\":\"$script\",\"sha256_main\":\"$s\"}"
    }
    # shellcheck disable=SC2086
    build GATE5 tests/run_numeric_gates.sh -I"$PHYS/forge" -- tests/test_omega_numeric.c src/omega_numeric.c src/omega_numeric_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric_provenance.c $BLK $FORGE
    # shellcheck disable=SC2086
    build REDUCE tests/run_reduce_chip.sh -I"$PHYS/forge" -- tests/test_omega_reduce.c src/omega_numeric_reduce.c src/omega_numeric_reduce_gb10.c src/omega_numeric.c src/omega_numeric_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric_provenance.c $BLK $FORGE
    # shellcheck disable=SC2086
    build LDST tools/run_numeric_ldst_chip.sh -pthread -- tests/test_omega_ldst_gb10.c src/omega_numeric_ldst_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c $PHYS/nvrm/nvrm.c $PHYS/m16/m16_native.c
    # shellcheck disable=SC2086
    build TRANSC tools/chip_run.sh+tools/manifests/numeric_transc.chiprun -fno-fast-math -pthread -- tests/test_omega_numeric_transc_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric_transc.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c $PHYS/nvrm/nvrm.c $PHYS/m16/m16_native.c
fi

body="{\"schema\":\"AIEN_E1_EQUIVALENCE_MANIFEST_V1\",\"candidate_git_commit\":\"$CAND\",\"main_git_commit\":\"$MAIN\""
body+=",\"whole_tree_identical\":$([ "$(git rev-parse "$CAND^{tree}")" = "$(git rev-parse "$MAIN^{tree}")" ] && echo true || echo false)"
body+=",\"files_equal\":$n_eq,\"files_differ\":$n_ne,\"chip_files_differ\":$chip_ne,\"files\":[$entries]"
[ -z "$bins" ] || body+=",\"rebuilt_chip_binaries\":[$bins]"
body+=",\"timestamp_utc\":\"$(date -u +%Y-%m-%dT%H:%M:%SZ)\"}"
if [ -n "$OUT" ]; then printf '%s\n' "$body" | jq . > "$OUT" && echo "manifest: $OUT (equal=$n_eq differ=$n_ne chip_differ=$chip_ne)"
else printf '%s\n' "$body" | jq .; fi
