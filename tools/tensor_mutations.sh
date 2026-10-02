#!/bin/sh
# M20 OMEGA_TENSOR mutation sweep, a thin adapter over tools/mutation_runner.sh.
# For each rule marked MUT:<name> in src/tensor/*.c the runner copies the tree
# to a scratch directory, breaks that one rule, rebuilds the CPU test there and
# requires the test to FAIL. A mutation that does not apply, does not build or
# is not caught fails the sweep. The adapter maps the runner verdicts back to
# the legacy stdout lines. CPU only, no device. Shell only (no Python).
set -u
cd "$(dirname "$0")/.." || exit 2
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/tensor-mut.XXXXXX") || exit 2
trap 'rm -rf "$SCRATCH"' EXIT INT TERM
DEPS="src/omega_numeric_reduce.c src/omega_numeric_transc.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c \
src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c"
FLAGS="-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -ffp-contract=off -O2 -DOMEGA_NUMERIC_CPU_ONLY -DOMEGA_TENSOR_TEST_HOOKS"

# name|file|sed expression (applied only to the line carrying MUT:<name>)
# Reduction order and padding mutations live in the E1 WP-D suite
# (test-numeric-reduce-cpu, RED_DIFFERENT_ORDER_CAUGHT): the seam calls it.
MUTATIONS='RELEASE_BUMP|omega_tensor.c|s/(\*gen)++/(void)0/
STORAGE_GEN_CHECK|omega_tensor.c|s/ || s->gen != h.generation//
BCAST_ZERO_STRIDE|omega_tensor.c|s/o.strides\[d\] = 0;/o.strides[d] = in->strides[sd];/
PERMUTE_STRIDES|omega_tensor.c|s/in->strides\[perm\[d\]\]/in->strides[d]/
SLICE_OFFSET|omega_tensor.c|s/start\[d\], in->strides\[d\]/start[d], 1/
TRANSC_EXP2_LOG2_SWAP|omega_tensor_cpu.c|s/= omega_math_exp2, \(.*\)= omega_math_log2,/= omega_math_log2, \1= omega_math_exp2,/
UNARY_VIEW_STRIDE|omega_tensor.c|s/gather(s, .in, src);/dense_strides(in.rank, in.shape, in.strides); gather(s, \&in, src);/
TRANSC_EXP_LOG_SWAP|omega_tensor_cpu.c|s/= omega_math_exp, \(.*\)= omega_math_log,/= omega_math_log, \1= omega_math_exp,/
RELU_NEG_ZERO|omega_tensor.c|s/if (u >> 31) return 0U;/if (u >> 31) return u == 0x80000000U ? u : 0U;/
RELU_NAN_PAYLOAD|omega_tensor.c|s/return OMEGA_QNAN_BITS;/return u;/
TRANSC_RSQRT_ERF_SWAP|omega_tensor_cpu.c|s/= omega_math_rsqrt, \(.*\)= omega_math_erf,/= omega_math_erf, \1= omega_math_rsqrt,/
GELU_VIEW_STRIDE|omega_tensor.c|s/gather(s, .in, src);/if (op == OMEGA_TU_GELU) { dense_strides(in.rank, in.shape, in.strides); } gather(s, \&in, src);/
TRIG_SIN_COS_SWAP|omega_tensor_cpu.c|s/= omega_math_sin, \(.*\)= omega_math_cos,/= omega_math_cos, \1= omega_math_sin,/
TRIG_DOMAIN_NUMBER|omega_tensor_cpu.c|s/out\[i\] = f(a\[i\]);/{ out[i] = f(a[i]); if ((op == OMEGA_TU_SIN || op == OMEGA_TU_COS) \&\& out[i] != out[i]) out[i] = 0.0f; }/
ZEROS_NEG0|omega_tensor.c|s/shape, 0.0f, out)/shape, -0.0f, out)/
NEG_SUB|omega_tensor.c|s/return u ^ 0x80000000U;/return omega_float_to_bits(0.0f - omega_bits_to_float(u));/
NEG_NAN|omega_tensor.c|s/return 0x7fc00000U;/return u ^ 0x80000000U;/
FULL_CANON|omega_tensor.c|s/uint32_t u = omega_float_to_bits(value);/uint32_t u = canon_bits(OMEGA_DT_F32, (const uint8_t *)\&value);/
EMBED_START|omega_tensor.c|s/start\[d\], ds\[d\]/start[d] + 1, ds[d]/
EMBED_STEP|omega_tensor.c|s/ds\[d\], st, /ds[d], 1, /
CONCAT_AXIS_OFFSET|omega_tensor.c|s/aoff \* ds\[axis\]/aoff/
CMP_FALSE_NEG_ZERO|omega_tensor_cpu.c|s/zero = 0.0f;/zero = -0.0f;/
CMP_EQ_NAN_TRUE|omega_tensor.c|s/OMEGA_NOP_FSETP_EQ_SEL,/OMEGA_NOP_FSETP_EQU_SEL,/
WHERE_ACCEPTS_HALF|omega_tensor.c|s/u != OMEGA_TENSOR_MASK_FALSE_BITS)/u != OMEGA_TENSOR_MASK_FALSE_BITS \&\& u != 0x3f000000U)/
REDUCE_AXES_DESCENDING|omega_tensor.c|s/for (uint32_t d = in.rank; d-- > 0;)/for (uint32_t d = 0; d < in.rank; d++)/
REDUCE_AXES_KEEPDIMS|omega_tensor.c|s/keep\[n\] = keepdims;/keep[n] = keepdims \&\& false;/
SUM_TO_SHAPE_SIZE1|omega_tensor.c|s/ax\[n\] = d; keep\[n\] = true; n++;/(void)0;/'

STORE_MUTATIONS='TSTORE_HASH_VERIFY|s/memcmp(dig, sl->payload_sha, 32) != 0 || memcmp(vid, sl->value_id, 32) != 0/0/
TSTORE_SHORT_RECORD|s/len != HDR_BYTES + count \* REC_BYTES/0/
TSTORE_GEN_RESET|s/sl->gen = r->gen + 1;/sl->gen = 1;/
TSTORE_RENAME_BEFORE_WRITE|s/const char \*wpath = tmp;/const char *wpath = final;/
TSTORE_RECORD_SUM|s/memcmp(sum, in + REC_SUM, 32) != 0/0/
TSTORE_HEADER_SUM|s/memcmp(sum, st->jbuf + HDR_SUM, 32) != 0/0/'
STORE_SRCS="src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c"
# The runner reads its own env var ONLY; the legacy sweep never did.
unset ONLY
fail=0
total=0

# run_block ROWSFILE BUILDCMD TESTCMD: run the shared runner (marker reader,
# baseline check on) and print one legacy "MUTATION <name>: ..." line per
# mutant, in row order. Returns 3 if the unmutated baseline does not pass.
# Legacy notes that need no mapping beyond the verdict: KILLED note
# "<N> failing case(s), first: ..." becomes "caught (<N> failing checks)",
# "harness exit" (nonzero exit, no FAIL line) becomes "caught (0 failing
# checks)". The 5-line build-log head the old script printed on a build error
# is replaced by the runner's one-line build error note.
run_block() {
    rb_rows=$1
    "tools/mutation_runner.sh" -k marker -m "$rb_rows" -t "$3" -b "$2" -c "src tests" -f FAIL -B \
        > "$SCRATCH/rb.out" 2> "$SCRATCH/rb.err"
    if grep -q 'unmutated baseline does not pass' "$SCRATCH/rb.out"; then
        cat "$SCRATCH/rb.err"; return 3
    fi
    while IFS= read -r rb_l; do
        rb_id=${rb_l%% *}; rb_r=${rb_l#* }; rb_st=${rb_r%% *}
        rb_note=${rb_l#*\(}; rb_note=${rb_note%)}
        total=$((total + 1))
        case $rb_st in
            KILLED)
                rb_n=$(printf '%s\n' "$rb_note" | sed -n 's/^\([0-9][0-9]*\) failing case.*/\1/p')
                echo "MUTATION $rb_id: caught (${rb_n:-0} failing checks)" ;;
            SURVIVED*) echo "MUTATION $rb_id: NOT CAUGHT (tests still pass)"; fail=1 ;;
            *)
                case $rb_note in
                    "build failed"*) echo "$rb_note"; echo "MUTATION $rb_id: does not build (fix the sed expression)" ;;
                    *) echo "MUTATION $rb_id: NOT APPLIED (marker or pattern missing)" ;;
                esac
                fail=1 ;;
        esac
    done < "$SCRATCH/rb.err"
}

# Main block. Rows are name|file|sed expression; the file column is relative
# to src/tensor, the runner wants it relative to the tree root.
printf '%s\n' "$MUTATIONS" | sed 's#^\([A-Z0-9_]*\)|#\1|src/tensor/#' > "$SCRATCH/main.rows"
# Baseline: the unmutated build must PASS, otherwise every mutant would
# look "caught" (e.g. the test refusing an unsuitable FP environment).
if ! run_block "$SCRATCH/main.rows" \
    "gcc $FLAGS -Isrc -Isrc/tensor -o .tensor_t tests/test_omega_tensor.c src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c $DEPS" \
    ./.tensor_t; then
    echo "tensor mutation sweep: FAIL (unmutated baseline does not pass)"; exit 1
fi
# M20 receipt writer mutants (MUT: markers in tools/m20_receipt.sh), own sweep.
tools/m20_receipt_mutations.sh || fail=1

# Crash-safe storage lifetime (omega_tensor_store.c): each mutant rebuilds
# tests/test_omega_tensor_store.c against the mutated copy and must FAIL it.
# (Skipping the fsync before rename is not observable without power loss, so
# it is not a mutant here.) Rows are name|expr, the file is implied.
printf '%s\n' "$STORE_MUTATIONS" | sed 's#^\([A-Z0-9_]*\)|#\1|src/tensor/omega_tensor_store.c|#' > "$SCRATCH/store.rows"
if ! run_block "$SCRATCH/store.rows" \
    "gcc $FLAGS -Isrc -Isrc/tensor -o .tstore_t tests/test_omega_tensor_store.c src/tensor/omega_tensor_store.c $STORE_SRCS $DEPS" \
    ./.tstore_t; then
    echo "MUTATION store baseline: unmutated store test does not pass"; fail=1
fi

if [ "$fail" -ne 0 ]; then echo "tensor mutation sweep: FAIL"; exit 1; fi
echo "tensor mutation sweep: PASS ($total of $total mutations caught)"
