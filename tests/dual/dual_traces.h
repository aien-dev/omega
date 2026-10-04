/* DUAL-1b pre-registered scenario traces (evidence/DUAL/1b-replay/receipt.md,
 * section "Pre-registered: scenarios"). Every generator is a pure function of
 * its declared parameters and seed; the trace digest is the scenario identity. */
#ifndef DUAL_TRACES_H
#define DUAL_TRACES_H
#include "rx_dual_replay.h"

#define DUAL_TRACE_N 200u
#define DUAL_SD 100.0
#define DUAL_BUDGET 5000.0
#define DUAL_SCALE 1000.0

/* Scenario 1..6, 8, 9, 11, 12 by number; 7 by part ('A' or 'B'); 10 by resource index r (0..31). */
RxDualStatus dual_trace_constant(RxDualTraceKind kind, uint64_t seed, uint32_t n, uint64_t gen0, double est, double sd, RxDualTrace *out);
RxDualStatus dual_trace_scenario(int scenario, char part, uint32_t r, RxDualTrace *out);
#endif
