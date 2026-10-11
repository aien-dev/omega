/* PD-0 dev-only regime switch. See pd0_regime.h. */
#include "physics0/regime/pd0_regime.h"

#include <stdlib.h>

int pd0_world_switch_regime(pd0_world *w, int level, uint32_t regime, int64_t *max_change_ppm)
{
    if (regime == 0) return -1;
    pd0_gen ng;
    for (uint32_t attempt = 0; attempt < 16; attempt++) {   /* an invalid L6 draw (redraw cap) is skipped */
        uint64_t s = w->seed ^ ((uint64_t)(regime + attempt * 977u) * 0x9E3779B97F4A7C15ull);
        if (pd0_gen_init(&ng, level, s) != 0) continue;
        int64_t worst = 0; const pd0_crange *t; int n = pd0_gen_const_table(level, &t);
        for (int i = 0; i < n; i++) { int64_t o = w->gen.k[i], d = llabs(ng.k[i] - o); int64_t ppm = o ? (int64_t)((double)d * 1e6 / (double)(o < 0 ? -o : o)) : 0; if (ppm > worst) worst = ppm; }
        w->gen = ng; w->in_episode = 0; w->steps_used = 0; w->episodes_used = 0;
        if (max_change_ppm) *max_change_ppm = worst;
        return 0;
    }
    return -1;
}
