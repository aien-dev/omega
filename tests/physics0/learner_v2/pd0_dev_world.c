/* See pd0_dev_world.h. Links the generator; never linked into a learner. */
#include "pd0_dev_world.h"
#include "physics0/regime/pd0_regime.h"
#include <stdlib.h>

struct dev_world { pd0_world w; int level; };
dev_world *dev_world_open(int level, uint64_t seed)
{ dev_world *d = calloc(1, sizeof *d); if (!d) return NULL; d->level = level; if (pd0_world_init(&d->w, level, seed)) { free(d); return NULL; } return d; }
size_t dev_world_call(dev_world *d, const uint8_t *req, size_t n, uint8_t *resp, size_t cap) { return pd0_world_handle(&d->w, req, n, resp, cap); }
int dev_world_switch(dev_world *d, uint32_t regime, int64_t *ppm) { return pd0_world_switch_regime(&d->w, d->level, regime, ppm); }
void dev_world_close(dev_world *d) { free(d); }
