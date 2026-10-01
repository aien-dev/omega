#!/bin/sh
# M20 OMEGA_TENSOR mutation sweep. For each rule marked MUT:<name> in
# src/tensor/*.c, copy src/tensor to a scratch directory, break that one rule,
# rebuild the CPU test against the copy and require the test to FAIL.
# A mutation that does not apply, or that the tests do not catch, fails the
# sweep. CPU only, no device. Shell only (no Python).
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
FULL_CANON|omega_tensor.c|s/uint32_t u = omega_float_to_bits(value);/uint32_t u = canon_bits(OMEGA_DT_F32, (const uint8_t *)\&value);/'

# Baseline: the unmutated build must PASS, otherwise every mutant would
# look "caught" (e.g. the test refusing an unsuitable FP environment).
if ! gcc $FLAGS -Isrc -Isrc/tensor -o "$SCRATCH/base" tests/test_omega_tensor.c \
    src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c $DEPS \
    > "$SCRATCH/base.log" 2>&1 || ! "$SCRATCH/base" > "$SCRATCH/base.run" 2>&1; then
    head -5 "$SCRATCH/base.log" "$SCRATCH/base.run" 2>/dev/null
    echo "tensor mutation sweep: FAIL (unmutated baseline does not pass)"; exit 1
fi
fail=0
total=0
old_ifs=$IFS
IFS='
'
for m in $MUTATIONS; do
    IFS=$old_ifs
    IFS='|' read -r name file expr <<EOM
$m
EOM
    total=$((total + 1))
    rm -rf "$SCRATCH/tensor" && cp -r src/tensor "$SCRATCH/tensor"
    sed -i "/MUT:$name/ $expr" "$SCRATCH/tensor/$file"
    if cmp -s "src/tensor/$file" "$SCRATCH/tensor/$file"; then
        echo "MUTATION $name: NOT APPLIED (marker or pattern missing)"; fail=1; continue
    fi
    if ! gcc $FLAGS -Isrc -I"$SCRATCH/tensor" -o "$SCRATCH/t" tests/test_omega_tensor.c \
        "$SCRATCH/tensor/omega_tensor.c" "$SCRATCH/tensor/omega_tensor_cpu.c" \
        "$SCRATCH/tensor/omega_tensor_reduce_seam.c" $DEPS > "$SCRATCH/build.log" 2>&1; then
        head -5 "$SCRATCH/build.log"
        echo "MUTATION $name: does not build (fix the sed expression)"; fail=1; continue
    fi
    if "$SCRATCH/t" > "$SCRATCH/run.log" 2>&1; then
        echo "MUTATION $name: NOT CAUGHT (tests still pass)"; fail=1
    else
        echo "MUTATION $name: caught ($(grep -c '^FAIL' "$SCRATCH/run.log") failing checks)"
    fi
done
IFS=$old_ifs
# M20 receipt writer mutants (MUT: markers in tools/m20_receipt.sh), own sweep.
tools/m20_receipt_mutations.sh || fail=1

# Crash-safe storage lifetime (omega_tensor_store.c): each mutant rebuilds
# tests/test_omega_tensor_store.c against the mutated copy and must FAIL it.
# (Skipping the fsync before rename is not observable without power loss, so
# it is not a mutant here.)
STORE_MUTATIONS='TSTORE_HASH_VERIFY|s/memcmp(dig, sl->payload_sha, 32) != 0 || memcmp(vid, sl->value_id, 32) != 0/0/
TSTORE_SHORT_RECORD|s/len != HDR_BYTES + count \* REC_BYTES/0/
TSTORE_GEN_RESET|s/sl->gen = r->gen + 1;/sl->gen = 1;/
TSTORE_RENAME_BEFORE_WRITE|s/const char \*wpath = tmp;/const char *wpath = final;/
TSTORE_RECORD_SUM|s/memcmp(sum, in + REC_SUM, 32) != 0/0/
TSTORE_HEADER_SUM|s/memcmp(sum, st->jbuf + HDR_SUM, 32) != 0/0/'
STORE_SRCS="src/tensor/omega_tensor.c src/tensor/omega_tensor_cpu.c src/tensor/omega_tensor_reduce_seam.c"
if ! gcc $FLAGS -Isrc -Isrc/tensor -o "$SCRATCH/sbase" tests/test_omega_tensor_store.c \
    src/tensor/omega_tensor_store.c $STORE_SRCS $DEPS > "$SCRATCH/sbase.log" 2>&1 \
    || ! "$SCRATCH/sbase" > "$SCRATCH/sbase.run" 2>&1; then
    head -5 "$SCRATCH/sbase.log" "$SCRATCH/sbase.run" 2>/dev/null
    echo "MUTATION store baseline: unmutated store test does not pass"; fail=1
else
    IFS='
'
    for m in $STORE_MUTATIONS; do
        IFS=$old_ifs
        name=${m%%|*}
        expr=${m#*|}
        total=$((total + 1))
        cp src/tensor/omega_tensor_store.c "$SCRATCH/store.c"
        sed -i "/MUT:$name/ $expr" "$SCRATCH/store.c"
        if cmp -s src/tensor/omega_tensor_store.c "$SCRATCH/store.c"; then
            echo "MUTATION $name: NOT APPLIED (marker or pattern missing)"; fail=1; continue
        fi
        if ! gcc $FLAGS -Isrc -Isrc/tensor -o "$SCRATCH/st" tests/test_omega_tensor_store.c \
            "$SCRATCH/store.c" $STORE_SRCS $DEPS > "$SCRATCH/sbuild.log" 2>&1; then
            head -5 "$SCRATCH/sbuild.log"
            echo "MUTATION $name: does not build (fix the sed expression)"; fail=1; continue
        fi
        if "$SCRATCH/st" > "$SCRATCH/srun.log" 2>&1; then
            echo "MUTATION $name: NOT CAUGHT (tests still pass)"; fail=1
        else
            echo "MUTATION $name: caught ($(grep -c '^FAIL' "$SCRATCH/srun.log") failing checks)"
        fi
    done
    IFS=$old_ifs
fi

if [ "$fail" -ne 0 ]; then echo "tensor mutation sweep: FAIL"; exit 1; fi
echo "tensor mutation sweep: PASS ($total of $total mutations caught)"
