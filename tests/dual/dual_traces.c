#include "dual_traces.h"
#include <string.h>

static void tick(RxDualTrace *t, uint32_t i, uint64_t gen, double est, double sd)
{
    t->t[i].generation = gen; t->t[i].estimate = est; t->t[i].uncertainty = sd;
    t->t[i].calibrated = 0; t->t[i].evidence_verified = 1; t->t[i].regime_change = 0;
}
static void head(RxDualTrace *t, RxDualTraceKind kind, uint64_t seed, uint32_t n)
{
    memset(t, 0, sizeof *t);
    t->trace_kind = (uint32_t)kind; t->seed = seed; t->n_ticks = n;
}

RxDualStatus dual_trace_constant(RxDualTraceKind kind, uint64_t seed, uint32_t n, uint64_t gen0, double est, double sd, RxDualTrace *out)
{
    head(out, kind, seed, n);
    for (uint32_t i = 0; i < n; i++) tick(out, i, gen0 + i, est, sd);
    return rx_dual_check_trace(out);
}

RxDualStatus dual_trace_scenario(int scenario, char part, uint32_t r, RxDualTrace *out)
{
    const uint32_t N = DUAL_TRACE_N;
    uint64_t s;
    switch (scenario) {
    case 1: return dual_trace_constant(RX_DUAL_TRACE_CONSTANT_LOW, 0x1001, N, 1, 4000.0, DUAL_SD, out);
    case 2: return dual_trace_constant(RX_DUAL_TRACE_BINDING, 0x1002, N, 1, 5500.0, DUAL_SD, out);
    case 3:
        head(out, RX_DUAL_TRACE_STEP, 0x1003, N);
        for (uint32_t i = 0; i < N; i++) tick(out, i, i + 1, (i < 100) ? 4000.0 : 6000.0, DUAL_SD);
        break;
    case 4:
        head(out, RX_DUAL_TRACE_SQUARE, 0x1004, N);
        for (uint32_t i = 0; i < N; i++) tick(out, i, i + 1, ((i % 50) < 25) ? 6000.0 : 4000.0, DUAL_SD);
        break;
    case 5: return dual_trace_constant(RX_DUAL_TRACE_COMPETING_MEMORY, 0x1005, N, 1, 6000.0, 50.0, out);
    case 6: return dual_trace_constant(RX_DUAL_TRACE_COMPETING_LATENCY, 0x1005, N, 1, 4900.0, 50.0, out);
    case 7: /* two chains: A = ticks 0..99 (generation 1..100), B = ticks 100..199 (generation 101..200) */
        if (part == 'A') return dual_trace_constant(RX_DUAL_TRACE_CAPACITY_REDUCTION, 0x1007, 100, 1, 4500.0, DUAL_SD, out);
        if (part == 'B') return dual_trace_constant(RX_DUAL_TRACE_CAPACITY_REDUCTION, 0x1007, 100, 101, 4500.0, DUAL_SD, out);
        return RX_DUAL_ERR_RANGE;
    case 8:
        head(out, RX_DUAL_TRACE_REGIME_CHANGE, 0x1008, N);
        for (uint32_t i = 0; i < N; i++) { tick(out, i, i + 1, 5500.0, DUAL_SD); out->t[i].regime_change = (i >= 80 && i < 120) ? 1u : 0u; }
        break;
    case 9:
        head(out, RX_DUAL_TRACE_BURSTY, 0x1009, N);
        s = 0x1009;
        for (uint32_t i = 0; i < N; i++) {
            double est = 4200.0;
            int burst = (rx_dual_xorshift64s(&s) & 7u) == 0u;                 /* probability 1/8 */
            double amp = 800.0 + 1600.0 * rx_dual_xorshift_unit(&s);          /* uniform [800, 2400) */
            if (burst) est += amp;
            tick(out, i, i + 1, est, DUAL_SD);
        }
        break;
    case 10: {
        double off;
        if (r >= RX_DUAL_MAX_RESOURCES) return RX_DUAL_ERR_RANGE;
        s = 0x100Au + r;
        off = -50.0 + 100.0 * rx_dual_xorshift_unit(&s);                        /* uniform [-50, 50) */
        return dual_trace_constant(RX_DUAL_TRACE_FANOUT, 0x100Au + r, N, 1, 5500.0 + off, DUAL_SD, out);
    }
    case 11:
        head(out, RX_DUAL_TRACE_KV_SATURATION, 0x100B, N);
        for (uint32_t i = 0; i < N; i++) tick(out, i, i + 1, 3000.0 + 4000.0 * (double)i / 199.0, DUAL_SD);
        break;
    case 12:
        head(out, RX_DUAL_TRACE_RECOVERY, 0x100C, N);
        for (uint32_t i = 0; i < N; i++) tick(out, i, i + 1, (i < 100) ? 6000.0 : 5000.0, DUAL_SD);
        break;
    default: return RX_DUAL_ERR_RANGE;
    }
    return rx_dual_check_trace(out);
}
