/*
 * OMEGA-NUMERIC-0 host-pure module: semantic reference tier, CPU realization
 * tier, Omega math sequences, op registry, kernel patch words and parity
 * comparators. No device access here (see src/omega_numeric_gb10.c).
 */
#include "omega_numeric.h"
#include "omega_blackwell_encoder.h"
#include "omega_blackwell_qmd.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if !defined(__aarch64__)
#error "OMEGA-NUMERIC-0 host tiers use AArch64 FP instructions (fmadd, fdiv, fsqrt, fminnm, scvtf, fcvtzs)"
#endif

bool omega_numeric_bits_equal(float a, float b) {
    if (omega_isnan(a) && omega_isnan(b)) {
        return true; /* NaN equivalence class */
    }
    return omega_float_to_bits(a) == omega_float_to_bits(b);
}

/* ---- Required FP environment ----------------------------------------------- */

uint64_t omega_numeric_read_fpcr(void) {
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v) : : "memory");
    return v;
}

bool omega_numeric_fpenv_ok(void) {
    return (omega_numeric_read_fpcr() & OMEGA_NUMERIC_FPCR_REQUIRED_CLEAR) == 0;
}

/* ---- Semantic Reference Tier (Zero Libm) --------------------------------- */

float omega_ref_fadd(float a, float b) { return a + b; }
float omega_ref_fsub(float a, float b) { return a - b; }
float omega_ref_fmul(float a, float b) { return a * b; }

float omega_ref_ffma(float a, float b, float c) {
    float r;
    __asm__("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(a), "w"(b), "w"(c));
    return r;
}

float omega_ref_fmin(float a, float b) {
    if (omega_isnan(a)) return b;
    if (omega_isnan(b)) return a;
    if (omega_iszero(a) && omega_iszero(b)) {
        return (omega_signbit(a) || omega_signbit(b)) ? omega_bits_to_float(0x80000000U) : 0.0f;
    }
    return (a < b) ? a : b;
}

float omega_ref_fmax(float a, float b) {
    if (omega_isnan(a)) return b;
    if (omega_isnan(b)) return a;
    if (omega_iszero(a) && omega_iszero(b)) {
        return (!omega_signbit(a) || !omega_signbit(b)) ? 0.0f : omega_bits_to_float(0x80000000U);
    }
    return (a > b) ? a : b;
}

bool omega_ref_fsetp_ge(float a, float b) {
    if (omega_isnan(a) || omega_isnan(b)) return false;
    return (a >= b);
}

float omega_ref_i2f(int32_t a) {
    return (float)a;
}

int32_t omega_ref_f2i(float a) {
    if (omega_isnan(a)) return 0;
    if (a >= 2147483647.0f) return 2147483647;
    if (a <= -2147483648.0f) return (int32_t)-2147483648LL;
    return (int32_t)a;
}

float omega_ieee_div(float x, float y) {
    float r;
    __asm__("fdiv %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y));
    return r;
}

float omega_ieee_sqrt(float x) {
    float r;
    __asm__("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
}

/* ---- Host seed used only by the raw-seed negative test ------------------- */
/* Magic-constant reciprocal seed. It is not a model of the hardware MUFU unit. */

float omega_numeric_host_rcp_seed(float y) {
    return omega_bits_to_float(0x7ef311c0U - omega_float_to_bits(y));
}

/* ---- Omega-defined division and square root -------------------------------
 * Both sequences use integer operations only (add, subtract, shift, compare,
 * select), which the SIMT integer pipe has. They use no MUFU seed and no FP
 * arithmetic, so the result does not depend on the FP rounding mode or on
 * flush-to-zero. The rounding step is written once (omega_round_pack) and
 * rounds to nearest, ties to even, including into the subnormal range and to
 * infinity on overflow. Result: correctly rounded IEEE a / b and sqrt(a).
 *
 * The earlier Newton sequences (magic-constant seed, two refinements, an
 * unfused residual) were up to 1 ULP off and overflowed to NaN when the
 * divisor was subnormal; the 4096-element corpus found 946 DIV and 201 SQRT
 * mismatches. */

/* Split finite nonzero |x| into m * 2^e with m in [2^23, 2^24). */
static void omega_unpack(uint32_t u, uint32_t *m, int32_t *e) {
    uint32_t be = (u >> 23) & 0xffU;
    uint32_t f = u & 0x007fffffU;
    if (be != 0) {
        *m = f | 0x00800000U;
        *e = (int32_t)be - 150;
        return;
    }
    int32_t ex = -149;
    while ((f & 0x00800000U) == 0) { f <<= 1; ex--; }
    *m = f;
    *e = ex;
}

/*
 * Round sig * 2^exp2 (plus a nonzero amount below the last bit of sig when
 * sticky) to FP32, ties to even. sig must be >= 2^25 so there are at least a
 * guard bit and one more bit below the 24-bit mantissa.
 */
static uint32_t omega_round_pack(uint32_t sign, uint64_t sig, int32_t exp2, bool sticky) {
    int32_t p = 63;
    while (((sig >> p) & 1U) == 0) p--;
    int32_t lead = p + exp2;             /* exponent of the leading bit */
    int32_t be = lead + 127;             /* biased exponent if normal */
    if (be >= 255) return sign | OMEGA_INF_POS;
    int32_t lsb = (be >= 1) ? lead - 23 : -149;
    int32_t s = lsb - exp2;              /* bits to drop, >= 2 by the sig bound */
    if (s > p + 1) return sign;          /* below half the smallest subnormal */
    uint64_t mant = sig >> s;
    bool guard = ((sig >> (s - 1)) & 1U) != 0;
    bool rest = sticky || (sig & ((1ULL << (s - 1)) - 1U)) != 0;
    if (guard && (rest || (mant & 1U))) mant++;
    /* Normal: mant in [2^23, 2^24]; a carry to 2^24 moves into the exponent.
     * Subnormal: mant in [0, 2^23]; 2^23 is the smallest normal. */
    uint64_t bits = (be >= 1) ? ((uint64_t)(be - 1) << 23) + mant : mant;
    if (bits >= OMEGA_INF_POS) return sign | OMEGA_INF_POS;
    return sign | (uint32_t)bits;
}

float omega_math_div(float x, float y) {
    if (omega_isnan(x) || omega_isnan(y)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    uint32_t sign = (omega_float_to_bits(x) ^ omega_float_to_bits(y)) & 0x80000000U;
    bool neg = sign != 0;

    if (omega_iszero(y)) {
        if (omega_iszero(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
        return neg ? omega_bits_to_float(OMEGA_INF_NEG) : omega_bits_to_float(OMEGA_INF_POS);
    }
    if (omega_iszero(x)) return neg ? -0.0f : 0.0f;
    if (omega_isinf(x)) {
        if (omega_isinf(y)) return omega_bits_to_float(OMEGA_QNAN_BITS);
        return neg ? omega_bits_to_float(OMEGA_INF_NEG) : omega_bits_to_float(OMEGA_INF_POS);
    }
    if (omega_isinf(y)) return neg ? -0.0f : 0.0f;

    uint32_t mx, my;
    int32_t ex, ey;
    omega_unpack(omega_float_to_bits(x), &mx, &ex);
    omega_unpack(omega_float_to_bits(y), &my, &ey);

    /* Restoring long division: q = floor(mx * 2^26 / my), 27 quotient bits.
     * mx / my > 1/2, so q >= 2^25. rem stays below 2^25. */
    uint32_t rem = mx, q = 0;
    for (int i = 0; i < 27; i++) {
        q <<= 1;
        if (rem >= my) { rem -= my; q |= 1U; }
        rem <<= 1;
    }
    return omega_bits_to_float(omega_round_pack(sign, q, ex - ey - 26, rem != 0));
}

float omega_math_sqrt(float x) {
    if (omega_isnan(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (omega_iszero(x)) return x;
    if (omega_signbit(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (omega_isinf(x)) return omega_bits_to_float(OMEGA_INF_POS);

    uint32_t m;
    int32_t e;
    omega_unpack(omega_float_to_bits(x), &m, &e);
    uint64_t mm = m;
    if (e & 1) { mm <<= 1; e -= 1; }       /* even exponent: x = mm * 2^e */

    /* Bitwise integer square root of M = mm * 2^28: s = floor(sqrt(M)),
     * s in [2^25, 2^27). x = M * 2^(e-28), sqrt(x) = sqrt(M) * 2^(e/2-14). */
    uint64_t op = mm << 28, s = 0, one = 1ULL << 62;
    while (one > op) one >>= 2;
    while (one != 0) {
        if (op >= s + one) { op -= s + one; s = (s >> 1) + one; }
        else s >>= 1;
        one >>= 2;
    }
    return omega_bits_to_float(omega_round_pack(0, s, e / 2 - 14, op != 0));
}

/* ---- Omega-Defined Exp Polynomial Sequence -------------------------------- */

float omega_math_exp(float x) {
    if (omega_isnan(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (x < -104.0f) return 0.0f;
    if (x > 88.722839f) return omega_bits_to_float(OMEGA_INF_POS);

    const float INV_LN2 = 1.4426950408889634f;
    const float LN2_HI  = 0.693145751953125f;
    const float LN2_LO  = 1.4286068203094172e-06f;

    float prod = x * INV_LN2;
    int32_t k = (prod >= 0.0f) ? (int32_t)(prod + 0.5f) : (int32_t)(prod - 0.5f);
    float fk = (float)k;
    float r = (x - fk * LN2_HI) - fk * LN2_LO;

    const float c2 = 0.5f;
    const float c3 = 0.1666666716f;
    const float c4 = 0.0416666679f;
    const float c5 = 0.0083333338f;

    float p = r * (c4 + r * c5);
    p = r * (c3 + p);
    p = r * (c2 + p);
    float poly = 1.0f + r * (1.0f + p);

    /*
     * Scale by 2^k. k spans [-150, 128] on the admitted domain, but a single
     * FP32 power of two only covers [-126, 127]. Split the scale so each
     * factor is a normal power of two: the first multiply is exact, the second
     * rounds once (into the subnormal range, or to the top binade).
     */
    if (k > 127) {
        return (poly * omega_bits_to_float(254U << 23)) *
               omega_bits_to_float((uint32_t)(k - 127 + 127) << 23);
    }
    if (k < -126) {
        return (poly * omega_bits_to_float((uint32_t)(k + 100 + 127) << 23)) *
               omega_bits_to_float((uint32_t)(-100 + 127) << 23);
    }
    return poly * omega_bits_to_float((uint32_t)(k + 127) << 23);
}

/* ---- Omega-Defined Log Polynomial Sequence -------------------------------- */

float omega_math_log(float x) {
    if (omega_isnan(x)) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (x < 0.0f) return omega_bits_to_float(OMEGA_QNAN_BITS);
    if (omega_iszero(x)) return omega_bits_to_float(OMEGA_INF_NEG);
    if (omega_isinf(x)) return omega_bits_to_float(OMEGA_INF_POS);

    int32_t e_adjust = 0;
    if (omega_issubnormal(x)) {
        x *= 8388608.0f; /* 2^23, exact: brings every subnormal into the normal range */
        e_adjust = -23;
    }

    uint32_t u = omega_float_to_bits(x);
    int32_t e = (int32_t)((u >> 23) & 0xff) - 127 + e_adjust;
    float m = omega_bits_to_float((u & 0x007fffff) | 0x3f800000);

    if (m > 1.41421356f) {
        m *= 0.5f;
        e += 1;
    }

    float z = omega_math_div(m - 1.0f, m + 1.0f);
    float z2 = z * z;

    const float c1 = 0.333333343f;
    const float c2 = 0.200000003f;
    const float c3 = 0.142857149f;

    float poly = 1.0f + z2 * (c1 + z2 * (c2 + z2 * c3));
    float ln_m = 2.0f * z * poly;

    const float LN2 = 0.6931471805599453f;
    return (float)e * LN2 + ln_m;
}

/* ---- Declared-Order Warp Reduction ---------------------------------------- */

float omega_warp_reduce_sum(const float warp_inputs[32]) {
    float val[32];
    memcpy(val, warp_inputs, sizeof(val));

    static const int deltas[5] = { 16, 8, 4, 2, 1 };
    for (int s = 0; s < 5; s++) {
        int d = deltas[s];
        for (int i = 0; i < 32; i++) {
            if (i + d < 32) {
                val[i] = val[i] + val[i + d];
            }
        }
    }
    return val[0];
}

/* ---- Op registry ---------------------------------------------------------- */

static const OmegaNumericOpInfo OP_TABLE[OMEGA_NOP_COUNT] = {
    { OMEGA_NOP_FADD, "FADD", 2, true, OMEGA_CMP_BIT_EXACT, "a + b (RNE, subnormals kept)", NULL },
    { OMEGA_NOP_FSUB, "FSUB", 2, true, OMEGA_CMP_BIT_EXACT, "a - b (RNE, subnormals kept)", NULL },
    { OMEGA_NOP_FMUL, "FMUL", 2, true, OMEGA_CMP_BIT_EXACT, "a * b (RNE, subnormals kept)", NULL },
    { OMEGA_NOP_FFMA, "FFMA", 3, true, OMEGA_CMP_BIT_EXACT, "fma(a, b, c), c uniform per launch", NULL },
    { OMEGA_NOP_FSETP_SEL, "FSETP_SEL", 2, true, OMEGA_CMP_INT_EXACT,
      "(a >= b, false if either is NaN) ? a : b, bits moved unchanged", NULL },
    { OMEGA_NOP_FSEL, "FSEL", 2, true, OMEGA_CMP_INT_EXACT,
      "(a >= 0, false if a is NaN) ? b : a, bits moved unchanged", NULL },
    { OMEGA_NOP_FMNMX_MIN, "FMNMX_MIN", 2, true, OMEGA_CMP_BIT_EXACT,
      "minNum(a, b), NaN yields the other operand, -0 < +0", NULL },
    { OMEGA_NOP_FMNMX_MAX, "FMNMX_MAX", 2, true, OMEGA_CMP_BIT_EXACT,
      "maxNum(a, b), NaN yields the other operand, -0 < +0", NULL },
    { OMEGA_NOP_I2FP, "I2FP", 1, true, OMEGA_CMP_BIT_EXACT, "(float)(int32 bits of a), RNE", NULL },
    { OMEGA_NOP_F2I, "F2I", 1, true, OMEGA_CMP_INT_EXACT,
      "int32 truncate of a, NaN -> 0, saturating", NULL },
    { OMEGA_NOP_MUFU_RCP, "MUFU_RCP", 1, true, OMEGA_CMP_SEED_BOUND,
      "seed for 1/a: relative error bound only", NULL },
    { OMEGA_NOP_MUFU_RSQ, "MUFU_RSQ", 1, true, OMEGA_CMP_SEED_BOUND,
      "seed for 1/sqrt(a): relative error bound only", NULL },
    { OMEGA_NOP_LDS_STS, "LDS_STS", 1, true, OMEGA_CMP_INT_EXACT,
      "a[i ^ 63]: each thread stores a[i] to shared word tid, BAR.SYNC, then loads word tid ^ 63 "
      "(mirror exchange inside each 64-thread CTA), bits moved unchanged", NULL },
    { OMEGA_NOP_SHFL_DOWN, "SHFL_DOWN", 1, true, OMEGA_CMP_INT_EXACT,
      "a[lane+1] within each 32-lane warp, lane 31 keeps its own value", NULL },
    { OMEGA_NOP_DIV, "DIV", 2, false, OMEGA_CMP_BIT_EXACT, "correctly rounded a / b (integer long division, RNE)",
      "no GB10 kernel for the Omega division sequence exists (the old path ran FADD)" },
    { OMEGA_NOP_SQRT, "SQRT", 1, false, OMEGA_CMP_BIT_EXACT, "correctly rounded sqrt(a) (integer square root, RNE)",
      "no GB10 kernel for the Omega square-root sequence exists (the old path ran FMUL)" },
    { OMEGA_NOP_EXP, "EXP", 1, false, OMEGA_CMP_BIT_EXACT, "omega_math_exp(a)",
      "no GB10 kernel for the Omega exp polynomial exists (the old path ran FADD)" },
    { OMEGA_NOP_LOG, "LOG", 1, false, OMEGA_CMP_BIT_EXACT, "omega_math_log(a)",
      "no GB10 kernel for the Omega log polynomial exists (the old path ran FADD)" },
    { OMEGA_NOP_REDUCE_SUM, "REDUCE_SUM", 1, true, OMEGA_CMP_BIT_EXACT,
      "declared pairwise-tree warp sum (" OMEGA_WARP_REDUCTION_DECLARED_ORDER "), lane 0 of each warp", NULL },
};

size_t omega_numeric_op_count(void) { return OMEGA_NOP_COUNT; }

const OmegaNumericOpInfo *omega_numeric_op_at(size_t index) {
    return index < OMEGA_NOP_COUNT ? &OP_TABLE[index] : NULL;
}

const OmegaNumericOpInfo *omega_numeric_op_find(const char *name) {
    if (!name) return NULL;
    if (strcmp(name, "LDS") == 0 || strcmp(name, "STS") == 0) name = "LDS_STS";
    if (strcmp(name, "FSETP") == 0) name = "FSETP_SEL";
    for (size_t i = 0; i < OMEGA_NOP_COUNT; i++) {
        if (strcmp(OP_TABLE[i].name, name) == 0) return &OP_TABLE[i];
    }
    return NULL;
}

const char *omega_numeric_compare_name(OmegaNumericCompare c) {
    switch (c) {
    case OMEGA_CMP_BIT_EXACT:  return "BIT_EXACT";
    case OMEGA_CMP_INT_EXACT:  return "INT_EXACT";
    case OMEGA_CMP_SEED_BOUND: return "SEED_BOUND";
    }
    return "UNKNOWN";
}

/* ---- Refused variants ------------------------------------------------------ */

static const struct { const char *name; const char *reason; } REFUSED_VARIANTS[] = {
    { "LDS.U8",     "only the 32-bit LDS form is encoded (the old byte-wide words were never a round trip)" },
    { "STS.U8",     "only the 32-bit STS form is encoded (the old byte-wide words were never a round trip)" },
    { "LDS.S8",     "only the 32-bit LDS form is encoded" },
    { "LDS.U16",    "only the 32-bit LDS form is encoded" },
    { "STS.U16",    "only the 32-bit STS form is encoded" },
    { "LDS.64",     "only the 32-bit LDS form is encoded (no 64-bit register pair form)" },
    { "STS.64",     "only the 32-bit STS form is encoded (no 64-bit register pair form)" },
    { "LDS.128",    "only the 32-bit LDS form is encoded (no 128-bit form)" },
    { "STS.128",    "only the 32-bit STS form is encoded (no 128-bit form)" },
    { "SHFL_UP",    "only SHFL.DOWN is encoded" },
    { "SHFL_BFLY",  "only SHFL.DOWN is encoded" },
    { "SHFL_IDX",   "only SHFL.DOWN is encoded" },
    { "REDUCE_MAX", "only the declared-order FP32 sum is encoded" },
    { "REDUCE_MIN", "only the declared-order FP32 sum is encoded" },
    { "REDUCE_SUM_BLOCK", "only the per-warp (32-lane) sum is encoded, not a CTA-wide one" },
};
#define REFUSED_VARIANT_COUNT (sizeof(REFUSED_VARIANTS) / sizeof(REFUSED_VARIANTS[0]))

size_t omega_numeric_refused_variant_count(void) { return REFUSED_VARIANT_COUNT; }
const char *omega_numeric_refused_variant_at(size_t index) {
    return index < REFUSED_VARIANT_COUNT ? REFUSED_VARIANTS[index].name : NULL;
}

void omega_numeric_launch_shape(size_t count, uint32_t *threads_per_block, uint32_t *grid_width) {
    uint32_t g = (uint32_t)((count + OMEGA_NUMERIC_CTA_THREADS - 1u) / OMEGA_NUMERIC_CTA_THREADS);
    if (threads_per_block) *threads_per_block = OMEGA_NUMERIC_CTA_THREADS;
    if (grid_width) *grid_width = g ? g : 1u;
}

static int refuse(char *err, size_t err_len, int rc, const char *fmt, const char *a, const char *b) {
    if (err && err_len) snprintf(err, err_len, fmt, a ? a : "", b ? b : "");
    return rc;
}

int omega_numeric_submit_check(const char *op_name,
                               const float *in_a, const float *in_b, const float *in_c,
                               const float *out_res, size_t count,
                               char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    for (size_t v = 0; op_name && v < REFUSED_VARIANT_COUNT; v++) {
        if (strcmp(op_name, REFUSED_VARIANTS[v].name) == 0) return refuse(err, err_len, OMEGA_NUMERIC_ERR_NOT_ENCODED, "variant %s is not encoded for GB10: %s", op_name, REFUSED_VARIANTS[v].reason); /* CHECK:variant_refused */
    }
    const OmegaNumericOpInfo *info = omega_numeric_op_find(op_name);
    if (!info) {
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS,
                      "unknown numeric op '%s'%s", op_name ? op_name : "(null)", NULL);
    }
    if (!info->gb10_encoded) {
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_NOT_ENCODED,
                      "op %s is not encoded for GB10: %s", info->name, info->not_encoded_reason);
    }
    if (!in_a || !out_res || count == 0 || count > OMEGA_NUMERIC_MAX_COUNT) {
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS,
                      "op %s: missing input/output buffer or count out of range%s", info->name, NULL);
    }
    if (info->arity >= 2 && !in_b) {
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS,
                      "op %s needs a second input%s", info->name, NULL);
    }
    if (info->op == OMEGA_NOP_FFMA) {
        if (!in_c) {
            return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS,
                          "op %s needs the c input%s", info->name, NULL);
        }
        uint32_t c0 = omega_float_to_bits(in_c[0]);
        for (size_t i = 1; i < count; i++) {
            if (omega_float_to_bits(in_c[i]) != c0) {
                return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS,
                              "op %s: c must be the same value in every element (it is carried "
                              "in the constant bank)%s", info->name, NULL);
            }
        }
    }
    if ((info->op == OMEGA_NOP_SHFL_DOWN || info->op == OMEGA_NOP_REDUCE_SUM) && (count % 32u) != 0) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "op %s: count must be a multiple of 32 (whole warps only)%s", info->name, NULL); /* CHECK:warp_count */
    if (info->op == OMEGA_NOP_LDS_STS && (count % OMEGA_NUMERIC_CTA_THREADS) != 0) return refuse(err, err_len, OMEGA_NUMERIC_ERR_OPERANDS, "op %s: count must be a multiple of the CTA size 64 (whole CTAs: every thread must reach BAR.SYNC)%s", info->name, NULL); /* CHECK:cta_count */
    /* The patch this op would submit, checked against a QMD for this launch. */
    OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX];
    int n = omega_numeric_patch_words(info->op, patch);
    if (n <= 0) {
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_NOT_ENCODED,
                      "op %s has no patch words%s", info->name, NULL);
    }
    uint32_t qmd1[OMEGA_BW_QMD_WORDS];
    OmegaBlackwellQmdConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.num_elements = (uint32_t)count;
    omega_numeric_launch_shape(count, &cfg.threads_per_block, &cfg.grid_width);
    if (omega_blackwell_build_qmd1(qmd1, &cfg) != 0) {
        return refuse(err, err_len, OMEGA_NUMERIC_ERR_BAD_ARGS, "op %s: launch QMD could not be built%s",
                      info->name, NULL);
    }
    return omega_numeric_check_patch(info->op, patch, n, qmd1, err, err_len);
}

/* ---- Kernel patch words ----------------------------------------------------
 * Register map of the vecadd baseline (src/omega_blackwell_encoder.c):
 *   R2 = a[i] and R5 = b[i] (both LDG.E, write barrier SB4), R1 = c[0x0][0x37c]
 *   (driver constant word 223, set per launch), R6:R7 = &out[i], R9 = result.
 * Control word fields (w3): stall [12:9], yield [13], write barrier [16:14],
 * read barrier [19:17], wait mask [25:20].
 *   0x010fca00  fixed-latency op, waits SB4 (the loads of a and b)
 *   0x010e2800  variable-latency op (MUFU, F2I, SHFL), waits SB4, sets SB0
 *   0x001fe200  STG that waits SB0 (after a variable-latency op)
 *   0x000fe200  STG with no wait (after a fixed-latency op, as in the baseline)
 * Every instruction word below was decoded with nvdisasm 13.0.88 -b SM121 on
 * 2026-09-30 (text in the provenance table); control words follow the
 * scoreboard pattern ptxas 13.0.88 emits for sm_121 for the same op followed
 * by STG. GB10 parity is established only by the chip receipt.
 */

#define W_EXIT  { 0x0000794dU, 0x00000000U, 0x03800000U, 0x000fea00U }
#define W_STG0  { 0x06007986U, 0x00000009U, 0x0c101904U, 0x000fe200U }
#define W_STG1  { 0x06007986U, 0x00000009U, 0x0c101904U, 0x001fe200U }

#define CTRL_FIXED 0x010fca00U
#define CTRL_VAR   0x010e2800U
#define CTRL_SHFL_NEXT 0x000e2400U  /* later SHFL.DOWN: sets SB0, no wait (FADD before it is fixed latency) */
#define CTRL_FADD_SB0  0x001fca00U  /* FADD that waits SB0 (the shuffled value)                         */

int omega_numeric_patch_words(OmegaNumericOp op,
                              OmegaNumericPatchInsn out[OMEGA_NUMERIC_PATCH_MAX]) {
    static const OmegaNumericPatchInsn STG0 = { W_STG0, "STG.E desc[UR4][R6.64], R9", NULL };
    static const OmegaNumericPatchInsn STG1 = { W_STG1, "STG.E desc[UR4][R6.64], R9 (waits SB0)", NULL };
    static const OmegaNumericPatchInsn EXIT = { W_EXIT, "EXIT", NULL };
    int n = 0;
#define PUT(w0_, w1_, w2_, w3_, text_, key_) \
    do { out[n].w[0] = (w0_); out[n].w[1] = (w1_); out[n].w[2] = (w2_); out[n].w[3] = (w3_); \
         out[n].text = (text_); out[n].provenance_key = (key_); n++; } while (0)
    switch (op) {
    case OMEGA_NOP_FADD:
        PUT(0x02097221U, 0x00000005U, 0x00000000U, CTRL_FIXED, "FADD R9, R2, R5", "FADD");
        return n;
    case OMEGA_NOP_FSUB:
        PUT(0x02097221U, 0x80000005U, 0x00000000U, CTRL_FIXED, "FADD R9, R2, -R5", "FSUB");
        return n;
    case OMEGA_NOP_FMUL:
        PUT(0x02097220U, 0x00000005U, 0x00400000U, CTRL_FIXED, "FMUL R9, R2, R5", "FMUL");
        return n;
    case OMEGA_NOP_FFMA:
        PUT(0x02097223U, 0x00000005U, 0x00000001U, CTRL_FIXED, "FFMA R9, R2, R5, R1", "FFMA");
        return n;
    case OMEGA_NOP_FSETP_SEL:
        PUT(0x0200720bU, 0x00000005U, 0x03f06000U, CTRL_FIXED,
            "FSETP.GE.AND P0, PT, R2, R5, PT", "FSETP_GE_R2_R5");
        PUT(0x02097208U, 0x00000005U, 0x00000000U, 0x000fca00U, "FSEL R9, R2, R5, P0", "FSEL_R2_R5_P0");
        out[n++] = STG0;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_FSEL:
        PUT(0x0200720bU, 0x000000ffU, 0x03f06000U, CTRL_FIXED,
            "FSETP.GE.AND P0, PT, R2, RZ, PT", "FSETP_GE_R2_RZ");
        PUT(0x05097208U, 0x00000002U, 0x00000000U, 0x000fca00U, "FSEL R9, R5, R2, P0", "FSEL_R5_R2_P0");
        out[n++] = STG0;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_FMNMX_MIN:
        PUT(0x02097209U, 0x00000005U, 0x03800000U, CTRL_FIXED, "FMNMX R9, R2, R5, PT", "FMNMX_MIN");
        return n;
    case OMEGA_NOP_FMNMX_MAX:
        PUT(0x02097209U, 0x00000005U, 0x07800000U, CTRL_FIXED, "FMNMX R9, R2, R5, !PT", "FMNMX_MAX");
        return n;
    case OMEGA_NOP_I2FP:
        PUT(0x00097245U, 0x00000002U, 0x00201400U, CTRL_FIXED, "I2FP.F32.S32 R9, R2", "I2FP");
        return n;
    case OMEGA_NOP_F2I:
        PUT(0x00097305U, 0x00000002U, 0x0020f100U, CTRL_VAR, "F2I.TRUNC.NTZ R9, R2", "F2I");
        out[n++] = STG1;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_MUFU_RCP:
        PUT(0x00097308U, 0x00000002U, 0x00001000U, CTRL_VAR, "MUFU.RCP R9, R2", "MUFU_RCP");
        out[n++] = STG1;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_MUFU_RSQ:
        PUT(0x00097308U, 0x00000002U, 0x00001400U, CTRL_VAR, "MUFU.RSQ R9, R2", "MUFU_RSQ");
        out[n++] = STG1;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_SHFL_DOWN:
        PUT(0x02097f89U, 0x08201f00U, 0x000e0000U, CTRL_VAR, "SHFL.DOWN PT, R9, R2, 0x1, 0x1f", "SHFL_DOWN_1");
        out[n++] = STG1;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_LDS_STS:
        /* Shared window: raw byte offsets from 0 through URZ, as NVK/NAK emit.
         * ptxas instead forms a base from SR_CgaCtaId and 0x400; if the chip
         * disagrees with the reference, that addressing is the first suspect. */
        PUT(0x00087819U, 0x00000002U, 0x000006ffU, CTRL_FIXED, "SHF.L.U32 R8, R0, 0x2, RZ", "SHF_L_R8_R0_2");
        PUT(0x080a7812U, 0x000000fcU, 0x078e3cffU, 0x000fca00U,
            "LOP3.LUT R10, R8, 0xfc, RZ, 0x3c, !PT", "LOP3_R10_R8_XOR_FC");
        PUT(0x08007988U, 0x00000002U, 0x080008ffU, 0x010fe200U, "STS [R8+URZ], R2", "STS_R8_R2");
        PUT(0x00007b1dU, 0x00000000U, 0x00010000U, 0x000fec00U, "BAR.SYNC.DEFER_BLOCKING 0x0", "BAR_SYNC_0");
        PUT(0x0a097984U, 0x000000ffU, 0x08000800U, 0x000e2800U, "LDS R9, [R10+URZ]", "LDS_R9_R10");
        out[n++] = STG1;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_REDUCE_SUM:
        /* Declared order: val[i] += val[i + d] for d = 16, 8, 4, 2, 1. Lanes
         * with i + d >= 32 read their own value (clamp 0x1f) and never feed
         * lane 0, so lane 0 is the declared sum. Only lane 0 is compared. */
        PUT(0x02097f89U, 0x0a001f00U, 0x000e0000U, CTRL_VAR, "SHFL.DOWN PT, R9, R2, 0x10, 0x1f", "SHFL_DOWN_16");
        PUT(0x02027221U, 0x00000009U, 0x00000000U, CTRL_FADD_SB0, "FADD R2, R2, R9", "FADD_R2_R2_R9");
        PUT(0x02097f89U, 0x09001f00U, 0x000e0000U, CTRL_SHFL_NEXT, "SHFL.DOWN PT, R9, R2, 0x8, 0x1f", "SHFL_DOWN_8");
        PUT(0x02027221U, 0x00000009U, 0x00000000U, CTRL_FADD_SB0, "FADD R2, R2, R9", "FADD_R2_R2_R9");
        PUT(0x02097f89U, 0x08801f00U, 0x000e0000U, CTRL_SHFL_NEXT, "SHFL.DOWN PT, R9, R2, 0x4, 0x1f", "SHFL_DOWN_4");
        PUT(0x02027221U, 0x00000009U, 0x00000000U, CTRL_FADD_SB0, "FADD R2, R2, R9", "FADD_R2_R2_R9");
        PUT(0x02097f89U, 0x08401f00U, 0x000e0000U, CTRL_SHFL_NEXT, "SHFL.DOWN PT, R9, R2, 0x2, 0x1f", "SHFL_DOWN_2");
        PUT(0x02027221U, 0x00000009U, 0x00000000U, CTRL_FADD_SB0, "FADD R2, R2, R9", "FADD_R2_R2_R9");
        PUT(0x02097f89U, 0x08201f00U, 0x000e0000U, CTRL_SHFL_NEXT, "SHFL.DOWN PT, R9, R2, 0x1, 0x1f", "SHFL_DOWN_1");
        PUT(0x02097221U, 0x00000009U, 0x00000000U, CTRL_FADD_SB0, "FADD R9, R2, R9", "FADD_R9_R2_R9");
        out[n++] = STG0;
        out[n++] = EXIT;
        return n;
    case OMEGA_NOP_DIV:
    case OMEGA_NOP_SQRT:
    case OMEGA_NOP_EXP:
    case OMEGA_NOP_LOG:
        return OMEGA_NUMERIC_ERR_NOT_ENCODED;
    case OMEGA_NOP_COUNT:
        break;
    }
#undef PUT
    return OMEGA_NUMERIC_ERR_BAD_ARGS;
}

int omega_numeric_build_kernel(OmegaNumericOp op, uint8_t *code, size_t code_len, size_t *out_len) {
    OmegaNumericPatchInsn patch[OMEGA_NUMERIC_PATCH_MAX];
    int n = omega_numeric_patch_words(op, patch);
    if (n <= 0) return n < 0 ? n : OMEGA_NUMERIC_ERR_BAD_ARGS;
    size_t len = 0;
    if (omega_blackwell_encode_vecadd(code, code_len, &len) != 0) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (len < OMEGA_NUMERIC_PATCH_OFFSET + (size_t)n * 16u) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    for (int i = 0; i < n; i++) {
        memcpy(code + OMEGA_NUMERIC_PATCH_OFFSET + (size_t)i * 16u, patch[i].w, 16);
    }
    if (out_len) *out_len = len;
    return OMEGA_NUMERIC_OK;
}

/* ---- Structural patch check (before any device is touched) ----------------
 * Each refusal is one line ending in a CHECK:<name> marker so that
 * tools/numeric_check_sweep.sh can delete it and prove a test notices. */

static int bad(char *err, size_t err_len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static int bad(char *err, size_t err_len, const char *fmt, ...) {
    if (err && err_len) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(err, err_len, fmt, ap);
        va_end(ap);
    }
    return OMEGA_NUMERIC_ERR_OPERANDS;
}

#define INSN_OP(p)   ((p).w[0] & 0xffffu)
#define INSN_DST(p)  (((p).w[0] >> 16) & 0xffu)
#define INSN_SRCA(p) (((p).w[0] >> 24) & 0xffu)
#define INSN_SRCB(p) ((p).w[1] & 0xffu)
#define CTRL_WBAR(w3) (((w3) >> 14) & 7u)
#define CTRL_WAIT(w3) (((w3) >> 20) & 0x3fu)

#define OPC_STS  0x7988u
#define OPC_LDS  0x7984u
#define OPC_BAR  0x7b1du
#define OPC_SHF  0x7819u
#define OPC_LOP3 0x7812u
#define OPC_SHFL 0x7f89u
#define OPC_FADD 0x7221u
#define OPC_STG  0x7986u
#define W_STG_WORD0 0x06007986u /* STG.E desc[UR4][R6.64], R9: address R6:R7, predicate PT */
#define W_STG_WORD1 0x00000009u /* value R9, offset 0 */
#define W_STG_WORD2 0x0c101904u /* 32-bit width, descriptor UR4 */

#define SHFL_DELTA(p) (((p).w[1] >> 21) & 0x1fu)
#define SHFL_CLAMP(p) (((p).w[1] >> 8) & 0x1fu)
#define SHFL_FIELDS   ((0x1fu << 21) | (0x1fu << 8))

int omega_numeric_check_patch(OmegaNumericOp op, const OmegaNumericPatchInsn *p, int n,
                              const uint32_t *qmd1, char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!p || !qmd1 || (unsigned)op >= OMEGA_NOP_COUNT) return bad(err, err_len, "check_patch: missing patch or QMD");
    if (n < 1 || n > (int)OMEGA_NUMERIC_PATCH_MAX) return bad(err, err_len, "patch length %d outside 1..%u", n, OMEGA_NUMERIC_PATCH_MAX); /* CHECK:patch_len */
    const char *name = OP_TABLE[op].name;
    int sts = -1, lds = -1, bar = -1, shf = -1, lop = -1, n_sts = 0, n_lds = 0, n_bar = 0, n_shfl = 0;
    for (int t = 0; t < n; t++) {
        uint32_t o = INSN_OP(p[t]);
        /* a result that arrives through a scoreboard is waited on by the next instruction */
        if (CTRL_WBAR(p[t].w[3]) <= 5u && (t + 1 >= n || !(CTRL_WAIT(p[t + 1].w[3]) & (1u << CTRL_WBAR(p[t].w[3]))))) return bad(err, err_len, "%s: instruction %d sets SB%u but the next instruction does not wait on it", name, t, CTRL_WBAR(p[t].w[3])); /* CHECK:scoreboard_wait */
        if (o == OPC_STS) {
            if ((p[t].w[0] & 0x00ffffffu) != 0x00007988u || (p[t].w[1] & ~0xffu) != 0 || p[t].w[2] != 0x080008ffu) return bad(err, err_len, "%s: STS at %d is not the encoded 32-bit STS [Rx+URZ] form", name, t); /* CHECK:sts_form */
            sts = t; n_sts++;
        } else if (o == OPC_LDS) {
            if ((p[t].w[0] & 0x0000ffffu) != 0x00007984u || p[t].w[1] != 0x000000ffu || p[t].w[2] != 0x08000800u) return bad(err, err_len, "%s: LDS at %d is not the encoded 32-bit LDS Rd, [Rx+URZ] form", name, t); /* CHECK:lds_form */
            lds = t; n_lds++;
        } else if (o == OPC_BAR) {
            if (p[t].w[0] != 0x00007b1du || p[t].w[1] != 0 || p[t].w[2] != 0x00010000u) return bad(err, err_len, "%s: barrier at %d is not BAR.SYNC 0", name, t); /* CHECK:bar_form */
            bar = t; n_bar++;
        } else if ((p[t].w[0] & 0x0fffu) == (OPC_STG & 0x0fffu)) {
            if (p[t].w[0] != W_STG_WORD0 || p[t].w[1] != W_STG_WORD1 || p[t].w[2] != W_STG_WORD2) return bad(err, err_len, "%s: store at %d is not the unpredicated STG.E desc[UR4][R6.64], R9 (32-bit, no offset)", name, t); /* CHECK:stg_form */
        } else if (o == OPC_SHF) {
            shf = t;
        } else if (o == OPC_LOP3) {
            lop = t;
        } else if (o == OPC_SHFL) {
            if ((p[t].w[0] & 0x0000ffffu) != 0x00007f89u || (p[t].w[1] & ~SHFL_FIELDS) != 0x08000000u || p[t].w[2] != 0x000e0000u) return bad(err, err_len, "%s: shuffle at %d is not the encoded SHFL.DOWN form", name, t); /* CHECK:shfl_form */
            if (SHFL_CLAMP(p[t]) != 0x1fu) return bad(err, err_len, "%s: shuffle at %d has clamp 0x%x, not 0x1f", name, t, SHFL_CLAMP(p[t])); /* CHECK:shfl_clamp */
            n_shfl++;
        }
    }

    bool uses_shared = n_sts || n_lds || n_bar;
    if (op == OMEGA_NOP_LDS_STS && !(n_sts && n_lds)) return bad(err, err_len, "%s: patch has no STS/LDS pair", name); /* CHECK:lds_sts_present */
    if (uses_shared) {
        uint32_t tx = qmd1[34] & 0xffffu;
        uint32_t barriers = (qmd1[35] >> 17) & 0x1fu;
        uint32_t shared_bytes = (qmd1[36] & 0x7ffu) << 7;
        uint32_t need = (tx - 1u) * 4u + 4u;
        if (n_sts != 1 || n_lds != 1 || n_bar != 1) return bad(err, err_len, "%s: expected one STS, one BAR.SYNC, one LDS (got %d, %d, %d)", name, n_sts, n_bar, n_lds); /* CHECK:shared_counts */
        if (!(sts < bar && bar < lds)) return bad(err, err_len, "%s: order must be STS, BAR.SYNC, LDS (at %d, %d, %d)", name, sts, bar, lds); /* CHECK:shared_order */
        if (barriers < 1u) return bad(err, err_len, "%s: QMD declares %u barriers; BAR.SYNC 0 needs at least 1", name, barriers); /* CHECK:qmd_barrier */
        if (tx != OMEGA_NUMERIC_CTA_THREADS) return bad(err, err_len, "%s: QMD CTA width %u is not the %u-thread CTA", name, tx, OMEGA_NUMERIC_CTA_THREADS); /* CHECK:qmd_cta */
        if (need > shared_bytes) return bad(err, err_len, "%s: needs %u bytes of shared memory, QMD declares %u", name, need, shared_bytes); /* CHECK:qmd_shared_size */
        if (shf < 0 || shf > sts || INSN_SRCA(p[shf]) != 0u || p[shf].w[1] != 2u || p[shf].w[2] != 0x000006ffu) return bad(err, err_len, "%s: STS address is not SHF.L.U32 of R0 (tid.x) by 2", name); /* CHECK:addr_shift */
        if (INSN_SRCA(p[sts]) != INSN_DST(p[shf])) return bad(err, err_len, "%s: STS address register R%u is not the shifted tid R%u", name, INSN_SRCA(p[sts]), INSN_DST(p[shf])); /* CHECK:sts_addr_reg */
        if (INSN_SRCB(p[sts]) != 2u) return bad(err, err_len, "%s: STS stores R%u, not the input value R2 (a[i])", name, INSN_SRCB(p[sts])); /* CHECK:sts_value_reg */
        if (lop < 0 || lop > lds || INSN_SRCA(p[lop]) != INSN_DST(p[shf]) || p[lop].w[2] != 0x078e3cffu) return bad(err, err_len, "%s: LDS address is not LOP3 XOR of the shifted tid", name); /* CHECK:lop3_form */
        if (p[lop].w[1] != (tx - 1u) * 4u) return bad(err, err_len, "%s: LDS address mask 0x%x is not (CTA-1)*4 = 0x%x (out of bounds or wrong partner)", name, p[lop].w[1], (tx - 1u) * 4u); /* CHECK:lop3_mask */
        if (INSN_SRCA(p[lds]) != INSN_DST(p[lop]) || INSN_DST(p[lds]) != 9u) return bad(err, err_len, "%s: LDS must load [R%u] into R9", name, INSN_DST(p[lop])); /* CHECK:lds_regs */
    }

    if (op == OMEGA_NOP_SHFL_DOWN) {
        int t = 0;
        while (t < n && INSN_OP(p[t]) != OPC_SHFL) t++;
        if (n_shfl != 1 || t >= n || SHFL_DELTA(p[t]) != 1u) return bad(err, err_len, "%s: expected one SHFL.DOWN by 1", name); /* CHECK:shfl_down_1 */
    }
    if (op == OMEGA_NOP_REDUCE_SUM) {
        static const uint32_t DELTA[5] = { 16u, 8u, 4u, 2u, 1u };
        if (n_shfl != 5 || n != 12) return bad(err, err_len, "%s: expected five SHFL.DOWN + FADD pairs, STG, EXIT (got %d shuffles, %d instructions)", name, n_shfl, n); /* CHECK:reduce_shape */
        uint32_t acc = INSN_SRCA(p[0]);
        for (int s = 0; s < 5; s++) {
            const OmegaNumericPatchInsn *sh = &p[2 * s], *fa = &p[2 * s + 1];
            if (INSN_OP(*sh) != OPC_SHFL || SHFL_DELTA(*sh) != DELTA[s]) return bad(err, err_len, "%s: step %d must be SHFL.DOWN by %u (declared order 16,8,4,2,1)", name, s, DELTA[s]); /* CHECK:reduce_delta_order */
            if (INSN_SRCA(*sh) != acc) return bad(err, err_len, "%s: step %d shuffles R%u, not the running sum R%u", name, s, INSN_SRCA(*sh), acc); /* CHECK:reduce_shfl_src */
            if (INSN_OP(*fa) != OPC_FADD || (fa->w[1] & ~0xffu) != 0 || fa->w[2] != 0) return bad(err, err_len, "%s: step %d is not followed by a plain FADD (no negate, no modifiers)", name, s); /* CHECK:reduce_fadd_form */
            if (!(CTRL_WAIT(fa->w[3]) & (1u << CTRL_WBAR(sh->w[3]))) || CTRL_WBAR(sh->w[3]) > 5u) return bad(err, err_len, "%s: FADD of step %d does not wait on the shuffle", name, s); /* CHECK:reduce_fadd_wait */
            if (INSN_SRCA(*fa) != acc || INSN_SRCB(*fa) != INSN_DST(*sh)) return bad(err, err_len, "%s: FADD of step %d does not add the shuffled value to the running sum", name, s); /* CHECK:reduce_fadd_srcs */
            if (INSN_DST(*fa) != (s == 4 ? 9u : acc)) return bad(err, err_len, "%s: FADD of step %d writes R%u", name, s, INSN_DST(*fa)); /* CHECK:reduce_fadd_dst */
        }
        if (p[10].w[0] != W_STG_WORD0 || p[10].w[1] != W_STG_WORD1 || p[10].w[2] != W_STG_WORD2) return bad(err, err_len, "%s: the sum in R9 is not stored by STG.E desc[UR4][R6.64] (unpredicated, 32-bit)", name); /* CHECK:reduce_store */
    }
    return OMEGA_NUMERIC_OK;
}

/* ---- Reference and CPU realization tiers ---------------------------------- */

static float ref_fsetp_sel(float a, float b) { return omega_ref_fsetp_ge(a, b) ? a : b; }
static float ref_fsel(float a, float b) { return omega_ref_fsetp_ge(a, 0.0f) ? b : a; }

static float ref_shfl_down(const float *a, size_t i) {
    return ((i % 32u) == 31u) ? a[i] : a[i + 1];
}

bool omega_numeric_element_checked(OmegaNumericOp op, size_t index) {
    if (op == OMEGA_NOP_REDUCE_SUM) return (index % 32u) == 0;
    return true;
}

static bool needs_b(OmegaNumericOp op) {
    return OP_TABLE[op].arity >= 2;
}

int omega_numeric_reference(OmegaNumericOp op, const float *a, const float *b,
                            const float *c, float *out, size_t count) {
    if ((unsigned)op >= OMEGA_NOP_COUNT || !a || !out) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    if (needs_b(op) && !b) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (op == OMEGA_NOP_FFMA && !c) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if ((op == OMEGA_NOP_SHFL_DOWN || op == OMEGA_NOP_REDUCE_SUM) && (count % 32u) != 0)
        return OMEGA_NUMERIC_ERR_OPERANDS;
    if (op == OMEGA_NOP_LDS_STS && (count % OMEGA_NUMERIC_CTA_THREADS) != 0) return OMEGA_NUMERIC_ERR_OPERANDS;
    for (size_t i = 0; i < count; i++) {
        float r = 0.0f;
        switch (op) {
        case OMEGA_NOP_FADD: r = omega_ref_fadd(a[i], b[i]); break;
        case OMEGA_NOP_FSUB: r = omega_ref_fsub(a[i], b[i]); break;
        case OMEGA_NOP_FMUL: r = omega_ref_fmul(a[i], b[i]); break;
        case OMEGA_NOP_FFMA: r = omega_ref_ffma(a[i], b[i], c[i]); break;
        case OMEGA_NOP_FSETP_SEL: r = ref_fsetp_sel(a[i], b[i]); break;
        case OMEGA_NOP_FSEL: r = ref_fsel(a[i], b[i]); break;
        case OMEGA_NOP_FMNMX_MIN: r = omega_ref_fmin(a[i], b[i]); break;
        case OMEGA_NOP_FMNMX_MAX: r = omega_ref_fmax(a[i], b[i]); break;
        case OMEGA_NOP_I2FP: r = omega_ref_i2f((int32_t)omega_float_to_bits(a[i])); break;
        case OMEGA_NOP_F2I: r = omega_bits_to_float((uint32_t)omega_ref_f2i(a[i])); break;
        case OMEGA_NOP_MUFU_RCP: r = omega_ieee_div(1.0f, a[i]); break;
        case OMEGA_NOP_MUFU_RSQ: r = omega_ieee_div(1.0f, omega_ieee_sqrt(a[i])); break;
        case OMEGA_NOP_LDS_STS: r = a[i ^ (OMEGA_NUMERIC_CTA_THREADS - 1u)]; break;
        case OMEGA_NOP_SHFL_DOWN: r = ref_shfl_down(a, i); break;
        case OMEGA_NOP_DIV: r = omega_ieee_div(a[i], b[i]); break;
        case OMEGA_NOP_SQRT: r = omega_ieee_sqrt(a[i]); break;
        case OMEGA_NOP_EXP: r = omega_math_exp(a[i]); break;
        case OMEGA_NOP_LOG: r = omega_math_log(a[i]); break;
        case OMEGA_NOP_REDUCE_SUM:
            r = ((i % 32u) == 0) ? omega_warp_reduce_sum(&a[i]) : 0.0f;
            break;
        case OMEGA_NOP_COUNT: return OMEGA_NUMERIC_ERR_BAD_ARGS;
        }
        out[i] = r;
    }
    return OMEGA_NUMERIC_OK;
}

/* Independent spelling of the declared order: explicit per-level arrays. */
static float cpu_tree_sum32(const float *a) {
    float l16[16], l8[8], l4[4], l2[2];
    for (int i = 0; i < 16; i++) l16[i] = a[i] + a[i + 16];
    for (int i = 0; i < 8; i++)  l8[i]  = l16[i] + l16[i + 8];
    for (int i = 0; i < 4; i++)  l4[i]  = l8[i] + l8[i + 4];
    for (int i = 0; i < 2; i++)  l2[i]  = l4[i] + l4[i + 2];
    return l2[0] + l2[1];
}

int omega_numeric_cpu_realize(OmegaNumericOp op, const float *a, const float *b,
                              const float *c, float *out, size_t count) {
    if ((unsigned)op >= OMEGA_NOP_COUNT || !a || !out) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    if (needs_b(op) && !b) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (op == OMEGA_NOP_FFMA && !c) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if ((op == OMEGA_NOP_SHFL_DOWN || op == OMEGA_NOP_REDUCE_SUM) && (count % 32u) != 0)
        return OMEGA_NUMERIC_ERR_OPERANDS;
    if (op == OMEGA_NOP_LDS_STS && (count % OMEGA_NUMERIC_CTA_THREADS) != 0) return OMEGA_NUMERIC_ERR_OPERANDS;
    for (size_t i = 0; i < count; i++) {
        float r = 0.0f;
        float x = a[i];
        float y = b ? b[i] : 0.0f;
        switch (op) {
        case OMEGA_NOP_FADD: __asm__ volatile("fadd %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); break;
        case OMEGA_NOP_FSUB: __asm__ volatile("fsub %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); break;
        case OMEGA_NOP_FMUL: __asm__ volatile("fmul %s0, %s1, %s2" : "=w"(r) : "w"(x), "w"(y)); break;
        case OMEGA_NOP_FFMA:
            __asm__ volatile("fmadd %s0, %s1, %s2, %s3" : "=w"(r) : "w"(x), "w"(y), "w"(c[i]));
            break;
        case OMEGA_NOP_FSETP_SEL:
            /* fcmp: unordered sets NZCV=0011, so GE (N==V) is false for NaN */
            __asm__ volatile("fcmp %s1, %s2\n\tfcsel %s0, %s1, %s2, ge"
                             : "=&w"(r) : "w"(x), "w"(y) : "cc");
            break;
        case OMEGA_NOP_FSEL:
            __asm__ volatile("fcmp %s1, #0.0\n\tfcsel %s0, %s2, %s1, ge"
                             : "=&w"(r) : "w"(x), "w"(y) : "cc");
            break;
        case OMEGA_NOP_FMNMX_MIN:
        case OMEGA_NOP_FMNMX_MAX: {
            /* FMINNM/FMAXNM follow IEEE 754-2008 minNum, which returns NaN for a
             * signaling NaN. Omega's minimum treats every NaN as missing data
             * (754-2019 minimumNumber), so quiet signaling NaNs first. */
            float qx = omega_isnan(x) ? omega_bits_to_float(omega_float_to_bits(x) | 0x00400000U) : x;
            float qy = omega_isnan(y) ? omega_bits_to_float(omega_float_to_bits(y) | 0x00400000U) : y;
            if (op == OMEGA_NOP_FMNMX_MIN) __asm__ volatile("fminnm %s0, %s1, %s2" : "=w"(r) : "w"(qx), "w"(qy));
            else                           __asm__ volatile("fmaxnm %s0, %s1, %s2" : "=w"(r) : "w"(qx), "w"(qy));
            break;
        }
        case OMEGA_NOP_I2FP: {
            int32_t iv = (int32_t)omega_float_to_bits(x);
            __asm__ volatile("scvtf %s0, %w1" : "=w"(r) : "r"(iv));
            break;
        }
        case OMEGA_NOP_F2I: {
            int32_t iv;
            __asm__ volatile("fcvtzs %w0, %s1" : "=r"(iv) : "w"(x));
            r = omega_bits_to_float((uint32_t)iv);
            break;
        }
        case OMEGA_NOP_MUFU_RCP: __asm__ volatile("fdiv %s0, %s1, %s2" : "=w"(r) : "w"(1.0f), "w"(x)); break;
        case OMEGA_NOP_MUFU_RSQ: {
            float s;
            __asm__ volatile("fsqrt %s0, %s1" : "=w"(s) : "w"(x));
            __asm__ volatile("fdiv %s0, %s1, %s2" : "=w"(r) : "w"(1.0f), "w"(s));
            break;
        }
        case OMEGA_NOP_LDS_STS: {
            /* independent spelling: mirror position inside the CTA */
            size_t base = i - (i % OMEGA_NUMERIC_CTA_THREADS);
            r = a[base + (OMEGA_NUMERIC_CTA_THREADS - 1u) - (i - base)];
            break;
        }
        case OMEGA_NOP_SHFL_DOWN: {
            size_t lane = i & 31u, base = i - lane;
            size_t src = lane + 1u > 31u ? lane : lane + 1u;
            r = a[base + src];
            break;
        }
        case OMEGA_NOP_DIV: r = omega_math_div(x, y); break;
        case OMEGA_NOP_SQRT: r = omega_math_sqrt(x); break;
        case OMEGA_NOP_EXP: r = omega_math_exp(x); break;
        case OMEGA_NOP_LOG: r = omega_math_log(x); break;
        case OMEGA_NOP_REDUCE_SUM: r = ((i & 31u) == 0) ? cpu_tree_sum32(&a[i]) : 0.0f; break;
        case OMEGA_NOP_COUNT: return OMEGA_NUMERIC_ERR_BAD_ARGS;
        }
        out[i] = r;
    }
    return OMEGA_NUMERIC_OK;
}

/* ---- Parity --------------------------------------------------------------- */

int omega_numeric_parity(OmegaNumericOp op, const float *expect, const float *got,
                         size_t count, OmegaParityTrace *trace) {
    if ((unsigned)op >= OMEGA_NOP_COUNT || !expect || !got || !trace) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    memset(trace, 0, sizeof(*trace));
    trace->first_index = -1;
    OmegaNumericCompare mode = OP_TABLE[op].compare;
    if (mode == OMEGA_CMP_SEED_BOUND) return OMEGA_NUMERIC_ERR_OPERANDS; /* never bit-compare MUFU */
    for (size_t i = 0; i < count; i++) {
        if (!omega_numeric_element_checked(op, i)) continue;
        trace->checked++;
        uint32_t ue = omega_float_to_bits(expect[i]);
        uint32_t ug = omega_float_to_bits(got[i]);
        if (mode == OMEGA_CMP_BIT_EXACT && omega_issubnormal(expect[i])) trace->subnormal_expected++;
        bool same = (mode == OMEGA_CMP_INT_EXACT) ? (ue == ug)
                                                  : omega_numeric_bits_equal(expect[i], got[i]);
        if (!same) {
            if (trace->mismatches == 0) {
                trace->first_index = (long)i;
                trace->first_expect = ue;
                trace->first_got = ug;
            }
            trace->mismatches++;
        }
    }
    return OMEGA_NUMERIC_OK;
}

static bool is_finite_normal(float x) {
    uint32_t e = (omega_float_to_bits(x) >> 23) & 0xffU;
    return e != 0 && e != 0xffU;
}

int omega_numeric_seed_bound(OmegaNumericOp op, const float *a, const float *got,
                             size_t count, OmegaParityTrace *trace) {
    if ((unsigned)op >= OMEGA_NOP_COUNT || !a || !got || !trace) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (OP_TABLE[op].compare != OMEGA_CMP_SEED_BOUND) return OMEGA_NUMERIC_ERR_OPERANDS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    memset(trace, 0, sizeof(*trace));
    trace->first_index = -1;
    const double bound = 1.0 / 1048576.0; /* 2^-20 relative */
    for (size_t i = 0; i < count; i++) {
        float x = a[i];
        if (!is_finite_normal(x) || (op == OMEGA_NOP_MUFU_RSQ && omega_signbit(x))) {
            trace->bound_skipped++;
            continue;
        }
        float e = (op == OMEGA_NOP_MUFU_RCP) ? omega_ieee_div(1.0f, x)
                                             : omega_ieee_div(1.0f, omega_ieee_sqrt(x));
        if (!is_finite_normal(e)) {
            trace->bound_skipped++;
            continue;
        }
        trace->checked++;
        double d = (double)got[i] - (double)e;
        if (d < 0) d = -d;
        double m = (double)e;
        if (m < 0) m = -m;
        if (omega_isnan(got[i]) || !(d <= m * bound)) {
            if (trace->out_of_bound == 0) {
                trace->first_index = (long)i;
                trace->first_expect = omega_float_to_bits(e);
                trace->first_got = omega_float_to_bits(got[i]);
            }
            trace->out_of_bound++;
        }
    }
    return OMEGA_NUMERIC_OK;
}

/* ---- Flush-to-zero model (negative test only) ----------------------------- */

static float ftz(float x) {
    return omega_issubnormal(x) ? omega_bits_to_float(omega_float_to_bits(x) & 0x80000000U) : x;
}

int omega_numeric_reference_ftz(OmegaNumericOp op, const float *a, const float *b,
                                 const float *c, float *out, size_t count) {
    if ((unsigned)op >= OMEGA_NOP_COUNT || !a || !out) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (!omega_numeric_fpenv_ok()) return OMEGA_NUMERIC_ERR_FPENV;
    if (needs_b(op) && !b) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    if (op == OMEGA_NOP_FFMA && !c) return OMEGA_NUMERIC_ERR_BAD_ARGS;
    for (size_t i = 0; i < count; i++) {
        float x = ftz(a[i]);
        float y = b ? ftz(b[i]) : 0.0f;
        float r;
        switch (op) {
        case OMEGA_NOP_FADD: r = omega_ref_fadd(x, y); break;
        case OMEGA_NOP_FSUB: r = omega_ref_fsub(x, y); break;
        case OMEGA_NOP_FMUL: r = omega_ref_fmul(x, y); break;
        case OMEGA_NOP_FFMA: r = omega_ref_ffma(x, y, ftz(c[i])); break;
        case OMEGA_NOP_FMNMX_MIN: r = omega_ref_fmin(x, y); break;
        case OMEGA_NOP_FMNMX_MAX: r = omega_ref_fmax(x, y); break;
        default: return OMEGA_NUMERIC_ERR_OPERANDS; /* only arithmetic ops have an FTZ model */
        }
        out[i] = ftz(r);
    }
    return OMEGA_NUMERIC_OK;
}
