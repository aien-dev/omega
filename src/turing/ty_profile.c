/* Turing Yield measurement profile V0. The text here is the digested form of
 * docs/turing/TURING_YIELD_PROFILE_V0.md; changing any field changes the
 * profile digest, and a model or gain record made under V0 is then refused. */
#include "turing/ty_record.h"

#include <stdio.h>
#include <string.h>

/* Pre-registered from fit seeds only (profile doc sections 7 and 9). */
#define TY_V0_BASELINE_MASK (TY_F_PREV1)
#define TY_V0_CANDIDATE_MASK (TY_F_OP | TY_F_PREV1 | TY_F_PREV2 | TY_F_PREV3 | TY_F_PREV4)
#define TY_V0_MARGIN_UB 234602333141LL /* ceil(3 x sqrt(3) x 45149.240 bits), LOSO seeds 1-7 */

void ty_yprofile_v0(ty_yprofile *p) {
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "TY measurement profile v0");
    snprintf(p->dataset, sizeof p->dataset,
             "crumbline exp-20260927-rep10 control trace.ctr seeds 1-10 per manifest; fit seeds 1-7; "
             "held-out seeds 8-10 scored once");
    snprintf(p->x_t, sizeof p->x_t,
             "one outcome per CTR1 record, K=9: EXPAND pruned equiv/cost/frontier/step_cap, EXPAND unpruned "
             "failed/partial/improved, SUBMIT accept/reject");
    snprintf(p->side_info, sizeof p->side_info,
             "not coded, open to every model: op_index (SUBMIT=15), depth clipped at 7, crumb boundaries, crumb "
             "ordinal, event_index");
    snprintf(p->context_reset, sizeof p->context_reset,
             "per crumb: previous-outcome features restart at event_index 0");
    p->K = TY_OUT_K;
    p->qbits = TY_QBITS;
    p->floor_q = 1;
    snprintf(p->estimator, sizeof p->estimator,
             "ty.fit.kt16.mdl.v0: KT add-1/2 counts, 16-bit largest-remainder rows summing to 65536, floor 1/65536, "
             "row kept iff fit-data saving > row cost, unseen context uses the default order-0 row");
    snprintf(p->lm_code, sizeof p->lm_code,
             "TYM0 v0 bitstream; L(M) = exact bits = 104 + 16(K-1) + rows x (keybits + 16(K-1))");
    p->fit_rule = TY_FIT_MDL_PRUNE;
    snprintf(p->baseline_rule, sizeof p->baseline_rule,
             "argmin validation DL (fit seeds 1-6, score seed 7) over order-0, order-1 (prev outcome), op-only; chosen order-1");
    p->baseline_mask = TY_V0_BASELINE_MASK;
    snprintf(p->candidate_rule, sizeof p->candidate_rule,
             "argmin validation DL over op + optional depth + previous outcomes 1..h, h = 1..5 (10 models); "
             "chosen op+prev1..4; refit on seeds 1-7");
    p->candidate_mask = TY_V0_CANDIDATE_MASK;
    snprintf(p->pass_rule, sizeof p->pass_rule,
             "PASS iff pooled T - qerr > margin_ub and, for each held-out seed with full L(B) and L(C) charged "
             "to it, T - qerr > 0; otherwise FAIL and STOP");
    p->margin_ub = TY_V0_MARGIN_UB;
    p->qerr_milli_ub = TY_QERR_MILLI_UB_PER_SYMBOL;
    snprintf(p->energy_denominator, sizeof p->energy_denominator,
             "TY-7: T/J = T / E_C, E_C = idle-subtracted energy of scoring the held-out split with C alone; "
             "E_C - E_B not used (near zero or negative makes the ratio unbounded)");
}
