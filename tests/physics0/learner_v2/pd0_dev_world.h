/* Byte-level handle to an in-process PD-0 world for the v2 dev drivers (harness side). The driver sees
 * request and response BYTES only; the generator, constants and world struct stay inside pd0_dev_world.c.
 * The same bytes the pipe transport carries (spec 2.3). */
#ifndef PD0_DEV_WORLD_H
#define PD0_DEV_WORLD_H
#include <stddef.h>
#include <stdint.h>
typedef struct dev_world dev_world;
dev_world *dev_world_open(int level, uint64_t seed);               /* level 0..6, or 7 for the null world */
size_t dev_world_call(dev_world *w, const uint8_t *req, size_t n, uint8_t *resp, size_t cap);
int dev_world_switch(dev_world *w, uint32_t regime, int64_t *max_change_ppm);
void dev_world_close(dev_world *w);
#endif
