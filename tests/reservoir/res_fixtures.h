/* Reference topologies of Armendarez et al. (Zenodo 10.5281/zenodo.17296788,
 * zip md5 ad2c5057dfeafe890e624d8b1174f5d8), as data for the Gate 1 tests
 * and the Gate 2 comparison. Generated mechanically from the archive, not
 * typed by hand; node numbers are the reference's 1-based numbers.
 *
 * Lattices (nodeNeighbors tables; the reference symmetrizes them with
 * max(A, A.') and zeroes the diagonal, so a pair listed on one side only is
 * still an edge):
 *   13 nodes: Lorenz63SolverCustomRegression13Nodes.m:211-223, ground 8,
 *             inputs 1, 2, 13 (:30-31). sha256 c9e2ad2b...eedbf1
 *   25 nodes: Lorenz63SolverCustomRegression25Nodes.m:211-235 (its builder
 *             is named buildAdjacencyMatrix43 but builds 25 nodes, :208-209),
 *             ground 14, inputs 1, 2, 25 (:30-31). sha256 c1b31403...dece
 *   43 nodes: Lorenz63SolverCustomRegression.m:212-254, ground 21, inputs
 *             1, 28, 42 (:30-31). sha256 d291460f...afdcac
 * 50-node chain: conmat of NARMA .../Connected Reservoir/Length50.mat
 *   (sha256 ff4c9d14...2aae3), 49 undirected edges, a single path with
 *   permuted node numbers; ground = 50 (an interior node of the path).
 *   Length5/10/20.mat are the plain path 1-2-..-L with ground L, which
 *   omega_res_topology_chain(L, L - 1) builds. */
#ifndef RES_FIXTURES_H
#define RES_FIXTURES_H

#include <stdint.h>

/* neighbour lists, 1-based, count first */
static const uint8_t RES_FIX_HEX13_NB[13][7] = {
    {1, 3},
    {1, 6},
    {3, 1, 4, 7},
    {4, 3, 5, 7, 8},
    {4, 4, 8, 9, 6},
    {3, 2, 5, 9},
    {4, 3, 4, 8, 10},
    {6, 4, 5, 7, 9, 10, 11},
    {4, 5, 6, 8, 11},
    {4, 7, 8, 11, 12},
    {4, 8, 9, 10, 12},
    {3, 10, 11, 13},
    {1, 12},
};
/* neighbour lists, 1-based, count first */
static const uint8_t RES_FIX_HEX25_NB[25][7] = {
    {1, 3},
    {1, 7},
    {3, 1, 4, 8},
    {4, 3, 5, 8, 9},
    {4, 4, 6, 9, 10},
    {4, 5, 7, 10, 11},
    {3, 2, 6, 11},
    {5, 3, 4, 9, 12, 13},
    {6, 4, 5, 8, 10, 13, 14},
    {6, 5, 6, 9, 11, 14, 15},
    {5, 6, 7, 10, 15, 16},
    {3, 8, 13, 17},
    {6, 8, 9, 12, 14, 17, 18},
    {6, 9, 10, 13, 15, 18, 19},
    {6, 10, 11, 14, 16, 19, 20},
    {2, 15, 20},
    {4, 12, 13, 18, 21},
    {6, 13, 14, 17, 19, 21, 22},
    {6, 14, 15, 18, 20, 22, 23},
    {4, 15, 16, 19, 23},
    {3, 17, 18, 22},
    {5, 18, 19, 21, 23, 24},
    {4, 19, 20, 22, 24},
    {3, 22, 23, 25},
    {1, 24},
};
/* neighbour lists, 1-based, count first */
static const uint8_t RES_FIX_HEX43_NB[43][7] = {
    {1, 3},
    {3, 4, 5, 7},
    {3, 1, 6, 9},
    {4, 2, 6, 7, 10},
    {4, 2, 7, 8, 11},
    {5, 3, 4, 9, 10, 13},
    {6, 2, 4, 5, 10, 11, 14},
    {4, 5, 11, 12, 15},
    {4, 3, 6, 13, 16},
    {6, 4, 6, 7, 13, 14, 17},
    {6, 5, 7, 8, 14, 15, 18},
    {3, 8, 15, 19},
    {6, 6, 9, 10, 16, 17, 20},
    {6, 7, 10, 11, 17, 18, 21},
    {6, 8, 11, 12, 18, 19, 22},
    {4, 9, 13, 20, 24},
    {6, 10, 13, 14, 20, 21, 25},
    {6, 11, 14, 15, 21, 22, 26},
    {5, 12, 15, 22, 23, 27},
    {6, 13, 16, 17, 24, 25, 29},
    {6, 14, 17, 18, 25, 26, 30},
    {6, 15, 18, 19, 26, 27, 31},
    {3, 19, 27, 28},
    {4, 16, 20, 29, 32},
    {6, 17, 20, 21, 29, 30, 33},
    {6, 18, 21, 22, 30, 31, 34},
    {5, 19, 22, 23, 31, 35},
    {1, 23},
    {6, 20, 24, 25, 32, 33, 36},
    {6, 21, 25, 26, 33, 34, 37},
    {6, 22, 26, 27, 34, 35, 38},
    {4, 24, 29, 36, 39},
    {6, 25, 29, 30, 36, 37, 40},
    {6, 26, 30, 31, 37, 38, 41},
    {3, 27, 31, 38},
    {5, 29, 32, 33, 39, 40},
    {6, 30, 33, 34, 40, 41, 43},
    {4, 31, 34, 35, 41},
    {3, 32, 36, 42},
    {4, 33, 36, 37, 43},
    {4, 34, 37, 38, 43},
    {1, 39},
    {3, 37, 40, 41},
};
static const uint8_t RES_FIX_CHAIN50_EDGES[49][2] = {
    {1, 4}, {2, 5}, {3, 6}, {3, 7}, {5, 8}, {6, 9}, {7, 10}, {4, 11}, {8, 12}, {9, 12},
    {10, 13}, {13, 14}, {11, 15}, {14, 16}, {15, 17}, {16, 18}, {17, 19}, {18, 20}, {20, 25}, {19, 26},
    {21, 27}, {21, 28}, {22, 28}, {22, 29}, {23, 29}, {23, 30}, {24, 30}, {24, 31}, {25, 31}, {27, 32},
    {26, 33}, {32, 34}, {33, 35}, {35, 36}, {34, 37}, {36, 38}, {37, 39}, {39, 40}, {38, 41}, {40, 42},
    {41, 43}, {42, 44}, {43, 47}, {44, 48}, {45, 48}, {45, 49}, {46, 49}, {46, 50}, {47, 50},
};

#define RES_FIX_HEX13_GROUND 8u
#define RES_FIX_HEX25_GROUND 14u
#define RES_FIX_HEX43_GROUND 21u
#define RES_FIX_CHAIN50_GROUND 50u
static const uint8_t RES_FIX_HEX13_INPUTS[3] = {1, 2, 13};
static const uint8_t RES_FIX_HEX25_INPUTS[3] = {1, 2, 25};
static const uint8_t RES_FIX_HEX43_INPUTS[3] = {1, 28, 42};

#endif /* RES_FIXTURES_H */
