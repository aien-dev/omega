/* Exhaustive breadth-first exploration of the lifecycle model. */
#ifndef WORLD_EXPLORE_H
#define WORLD_EXPLORE_H
#include "world_model.h"

#define WX_MAX_TRACE 64
typedef struct {
    uint64_t states, transitions;
    uint32_t viol_seen;                     /* OR of every violation bit */
    int      trace_len[5];                  /* shortest counterexample per bit */
    WmOp     trace[5][WX_MAX_TRACE];
    int      truncated;                     /* state table full: not exhaustive */
} WxResult;

/* Bit index 0..4 for V_* bits. */
int  wx_bit_index(uint32_t bit);
int  wx_explore(const WmProfile *p, WxResult *res);
void wx_print_trace(const WmOp *t, int n);
#endif
