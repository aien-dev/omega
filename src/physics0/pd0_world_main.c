/* pd0-world <level 0..6|null> <seed>: the PD-0 world as its own process
 * (spec 2.4). Reads framed requests on stdin, writes framed responses on
 * stdout, exits at EOF. Nothing else is printed. */
#define PD0_WORLD_IMPL 1
#include "physics0/pd0_world.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: pd0-world <level 0..6|null> <seed>\n"); return 2; }
    int level = strcmp(argv[1], "null") == 0 ? PD0_LEVEL_NULL : atoi(argv[1]);
    static pd0_world w;
    if (pd0_world_init(&w, level, strtoull(argv[2], NULL, 0)) != 0) { fprintf(stderr, "pd0-world: bad level\n"); return 2; }
    return pd0_world_serve(&w, 0, 1) == 0 ? 0 : 1;
}
