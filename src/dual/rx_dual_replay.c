/* DUAL-1b: digest-bound traces and deterministic replay. */
#include "rx_dual_replay.h"
#include "sha256.h"

#include <math.h>
#include <string.h>

/* ---- generator ---- */
uint64_t rx_dual_xorshift64s(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}
double rx_dual_xorshift_unit(uint64_t *s) { return (double)(rx_dual_xorshift64s(s) >> 11) / 9007199254740992.0; }

/* ---- little-endian encoding (same conventions as rx_dual.c) ---- */
static void p_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void p_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void p_f64(uint8_t *p, double d)
{
    uint64_t bits;
    if (d == 0.0) d = 0.0; /* -0.0 -> +0.0 */
    memcpy(&bits, &d, 8);
    p_u64(p, bits);
}
static uint32_t g_u32(const uint8_t *p) { uint32_t v = 0; for (int i = 3; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static uint64_t g_u64(const uint8_t *p) { uint64_t v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static double g_f64(const uint8_t *p) { uint64_t bits = g_u64(p); double d; memcpy(&d, &bits, 8); return d; }

static RxDualStatus check_tick(const RxDualTraceTick *t)
{
    if (!isfinite(t->estimate) || !isfinite(t->uncertainty)) return RX_DUAL_ERR_NONFINITE;
    if (t->uncertainty < 0.0) return RX_DUAL_ERR_RANGE;
    if (t->calibrated > 1u || t->evidence_verified > 1u || t->regime_change > 1u) return RX_DUAL_ERR_RANGE;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_check_trace(const RxDualTrace *tr)
{
    RxDualStatus st;
    if (!tr) return RX_DUAL_ERR_NULL;
    if (tr->trace_kind < 1u || tr->trace_kind >= (uint32_t)RX_DUAL_TRACE_MAX_) return RX_DUAL_ERR_KIND;
    if (tr->n_ticks < 1u || tr->n_ticks > RX_DUAL_TRACE_MAX_TICKS) return RX_DUAL_ERR_RANGE;
    for (uint32_t i = 0; i < tr->n_ticks; i++)
        if ((st = check_tick(&tr->t[i])) != RX_DUAL_OK) return st;
    return RX_DUAL_OK;
}

static void enc_header(const RxDualTrace *tr, uint8_t out[RX_DUAL_TRACE_HEADER_ENCODED])
{
    out[0] = (uint8_t)RX_DUAL_KIND_TRACE; out[1] = (uint8_t)RX_DUAL_FORMAT_VERSION;
    p_u32(out + 2, tr->trace_kind); p_u64(out + 6, tr->seed); p_u32(out + 14, tr->n_ticks);
}
static void enc_tick(const RxDualTraceTick *t, uint8_t out[RX_DUAL_TRACE_TICK_ENCODED])
{
    p_u64(out, t->generation); p_f64(out + 8, t->estimate); p_f64(out + 16, t->uncertainty);
    p_u32(out + 24, t->calibrated); p_u32(out + 28, t->evidence_verified); p_u32(out + 32, t->regime_change);
}

RxDualStatus rx_dual_encode_trace(const RxDualTrace *tr, uint8_t *buf, size_t cap, size_t *len)
{
    RxDualStatus st;
    size_t need;
    if (!buf || !len) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_trace(tr)) != RX_DUAL_OK) return st;
    need = RX_DUAL_TRACE_HEADER_ENCODED + (size_t)tr->n_ticks * RX_DUAL_TRACE_TICK_ENCODED;
    if (cap < need) return RX_DUAL_ERR_ENCODING;
    enc_header(tr, buf);
    for (uint32_t i = 0; i < tr->n_ticks; i++)
        enc_tick(&tr->t[i], buf + RX_DUAL_TRACE_HEADER_ENCODED + (size_t)i * RX_DUAL_TRACE_TICK_ENCODED);
    *len = need;
    return RX_DUAL_OK;
}

RxDualStatus rx_dual_decode_trace(const uint8_t *buf, size_t len, RxDualTrace *out)
{
    uint32_t n;
    if (!buf || !out) return RX_DUAL_ERR_NULL;
    if (len < RX_DUAL_TRACE_HEADER_ENCODED) return RX_DUAL_ERR_ENCODING;
    if (buf[0] != (uint8_t)RX_DUAL_KIND_TRACE) return RX_DUAL_ERR_KIND;
    if (buf[1] != (uint8_t)RX_DUAL_FORMAT_VERSION) return RX_DUAL_ERR_ENCODING;
    memset(out, 0, sizeof *out);
    out->trace_kind = g_u32(buf + 2); out->seed = g_u64(buf + 6); n = g_u32(buf + 14);
    if (n < 1u || n > RX_DUAL_TRACE_MAX_TICKS) return RX_DUAL_ERR_RANGE;
    if (len != RX_DUAL_TRACE_HEADER_ENCODED + (size_t)n * RX_DUAL_TRACE_TICK_ENCODED) return RX_DUAL_ERR_ENCODING;
    out->n_ticks = n;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *p = buf + RX_DUAL_TRACE_HEADER_ENCODED + (size_t)i * RX_DUAL_TRACE_TICK_ENCODED;
        RxDualTraceTick *t = &out->t[i];
        t->generation = g_u64(p); t->estimate = g_f64(p + 8); t->uncertainty = g_f64(p + 16);
        t->calibrated = g_u32(p + 24); t->evidence_verified = g_u32(p + 28); t->regime_change = g_u32(p + 32);
    }
    return rx_dual_check_trace(out);
}

RxDualStatus rx_dual_digest_trace(const RxDualTrace *tr, RxDualDigest *out)
{
    sha256_ctx ctx;
    uint8_t zero = 0, hdr[RX_DUAL_TRACE_HEADER_ENCODED], tick[RX_DUAL_TRACE_TICK_ENCODED];
    RxDualStatus st;
    if (!out) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_trace(tr)) != RX_DUAL_OK) return st;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)RX_DUAL_DOMAIN_TRACE, strlen(RX_DUAL_DOMAIN_TRACE));
    sha256_update(&ctx, &zero, 1);
    enc_header(tr, hdr);
    sha256_update(&ctx, hdr, sizeof hdr);
    for (uint32_t i = 0; i < tr->n_ticks; i++) { enc_tick(&tr->t[i], tick); sha256_update(&ctx, tick, sizeof tick); }
    sha256_final(&ctx, out->b);
    return RX_DUAL_OK;
}

/* ---- perf-stat CSV import (bytes in, no locale, no strtod) ---- */
static int field_eq(const uint8_t *p, size_t n, const char *s) { return strlen(s) == n && memcmp(p, s, n) == 0; }
static int parse_udec(const uint8_t *p, size_t n, double *out)
{
    double v = 0.0;
    if (n == 0 || n > 19) return 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 0;
        v = v * 10.0 + (double)(p[i] - '0');
    }
    *out = v;
    return 1;
}

RxDualStatus rx_dual_trace_from_perf_csv(const uint8_t *bytes, size_t len, const char *cpu, const char *event,
                                         RxDualTrace *out)
{
    size_t i = 0;
    uint32_t n = 0;
    if (!bytes || !cpu || !event || !out) return RX_DUAL_ERR_NULL;
    memset(out, 0, sizeof *out);
    while (i < len) {
        size_t eol = i, f[8], fn = 0, k;
        while (eol < len && bytes[eol] != '\n') eol++;
        if (eol > i && bytes[i] != '#') {
            /* field starts: f[0..fn), terminated by ',' or end of line; need at least 5 fields */
            f[fn++] = i;
            for (k = i; k < eol && fn < 8; k++) if (bytes[k] == ',') f[fn++] = k + 1;
            if (fn >= 5) {
                size_t c0 = f[1], c1 = f[2] - 1, v0 = f[2], v1 = f[3] - 1, e0 = f[4], e1 = (fn > 5) ? f[5] - 1 : eol;
                if (field_eq(bytes + c0, c1 - c0, cpu) && field_eq(bytes + e0, e1 - e0, event)) {
                    double v;
                    if (n >= RX_DUAL_TRACE_MAX_TICKS) return RX_DUAL_ERR_FULL;
                    if (!parse_udec(bytes + v0, v1 - v0, &v)) return RX_DUAL_ERR_ENCODING; /* "<not counted>" and friends */
                    out->t[n].generation = (uint64_t)n + 1u;
                    out->t[n].estimate = v;
                    out->t[n].uncertainty = 0.0;
                    out->t[n].calibrated = 0;          /* a counter is not a calibrated estimate */
                    out->t[n].evidence_verified = 1;
                    out->t[n].regime_change = 0;
                    n++;
                }
            }
        }
        i = eol + 1;
    }
    if (n == 0) return RX_DUAL_ERR_RANGE;
    out->trace_kind = RX_DUAL_TRACE_RECORDED;
    out->seed = 0;
    out->n_ticks = n;
    return rx_dual_check_trace(out);
}

/* ---- replay ---- */
static void derive(const RxDualDigest *trace, const char *tag, uint32_t i, RxDualDigest *out)
{
    sha256_ctx ctx; uint8_t ib[4];
    p_u32(ib, i);
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)tag, strlen(tag));
    sha256_update(&ctx, trace->b, RX_DUAL_DIGEST_SIZE);
    sha256_update(&ctx, ib, 4);
    sha256_final(&ctx, out->b);
}

RxDualStatus rx_dual_replay_run(const RxDualTrace *tr, const RxDualController *ctl, const RxDualResource *res,
                                RxDualClass cls, double budget, const RxDualDigest *budget_contract,
                                const RxDualDigest *calibration_ref, RxDualReplay *out)
{
    RxDualStatus st;
    RxDualConstraintState cur, nx;
    RxDualInput in;
    sha256_ctx ctx;
    uint8_t zero = 0, nb[4];
    if (!tr || !ctl || !res || !budget_contract || !out) return RX_DUAL_ERR_NULL;
    if ((st = rx_dual_check_trace(tr)) != RX_DUAL_OK) return st;
    memset(out, 0, sizeof *out);
    if ((st = rx_dual_digest_trace(tr, &out->trace)) != RX_DUAL_OK) return st;
    if ((st = rx_dual_digest_controller(ctl, &out->controller_id)) != RX_DUAL_OK) return st;
    for (uint32_t i = 0; i < tr->n_ticks; i++) {
        const RxDualTraceTick *t = &tr->t[i];
        memset(&in, 0, sizeof in);
        derive(&out->trace, "omega.dual.trace.estimate_ref", i, &in.estimate_ref);
        derive(&out->trace, "omega.dual.trace.evidence_root", 0, &in.evidence_root);
        in.estimate_kind = RX_DUAL_EST_ESTIMATED;
        in.unit = res->unit;
        in.estimate = t->estimate;
        in.uncertainty = t->uncertainty;
        in.generation = t->generation;
        in.evidence_verified = t->evidence_verified;
        in.regime_change = t->regime_change;
        if (t->calibrated) {
            if (!calibration_ref || rx_dual_digest_is_zero(calibration_ref)) return RX_DUAL_ERR_CALIBRATION;
            in.calibration_ref = *calibration_ref;
        }
        if (i == 0) st = rx_dual_init_state(res, cls, budget, budget_contract, &in, ctl, t->generation, &nx);
        else st = rx_dual_update(&cur, res, &in, ctl, t->generation, &nx);
        if (st != RX_DUAL_OK) return st;
        if ((st = rx_dual_digest_constraint(&nx, &out->record[i])) != RX_DUAL_OK) return st;
        out->lambda[i] = nx.lambda;
        out->estimate[i] = nx.estimate;
        out->generation[i] = nx.generation;
        out->state[i] = nx.lambda_state;
        cur = nx;
    }
    out->n = tr->n_ticks;
    out->last = cur;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)RX_DUAL_DOMAIN_REPLAY, strlen(RX_DUAL_DOMAIN_REPLAY));
    sha256_update(&ctx, &zero, 1);
    sha256_update(&ctx, out->controller_id.b, RX_DUAL_DIGEST_SIZE);
    sha256_update(&ctx, out->trace.b, RX_DUAL_DIGEST_SIZE);
    p_u32(nb, out->n);
    sha256_update(&ctx, nb, 4);
    for (uint32_t i = 0; i < out->n; i++) sha256_update(&ctx, out->record[i].b, RX_DUAL_DIGEST_SIZE);
    sha256_final(&ctx, out->replay.b);
    return RX_DUAL_OK;
}

/* ---- measures ---- */
uint32_t rx_dual_reversals(const double *x, uint32_t lo, uint32_t hi)
{
    uint32_t r = 0;
    int last = 0;
    if (!x || hi <= lo) return 0;
    for (uint32_t i = lo + 1; i < hi; i++) {
        double d = x[i] - x[i - 1];
        int s = (d > 0.0) ? 1 : (d < 0.0) ? -1 : 0;
        if (s == 0) continue;
        if (last != 0 && s != last) r++;
        last = s;
    }
    return r;
}

double rx_dual_peak_to_peak(const double *x, uint32_t lo, uint32_t hi)
{
    double mn, mx;
    if (!x || hi <= lo) return 0.0;
    mn = mx = x[lo];
    for (uint32_t i = lo + 1; i < hi; i++) { if (x[i] < mn) mn = x[i]; if (x[i] > mx) mx = x[i]; }
    return mx - mn;
}
