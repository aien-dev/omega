/*
 * omega_matvec_quad.h -- one more native realization of Omega's matvec (R10).
 *
 * The M12 family in omega_matvec.c is left exactly as qualified. Measured on
 * the DGX Spark (Neoverse V2), none of its three kinds beats the compiled
 * semantic reference: every element load post-increments its base register,
 * so each row is a serial chain of address updates.
 *
 * quad4 keeps four independent accumulators, loads at fixed offsets from a
 * base that moves once per four elements, and finishes N % 4 with the same
 * scalar tail the M12 kinds use. Same calling convention and semantics:
 * y[i] = sum_j A[i*N+j] * x[j] mod 2^64, for M >= 1 and N >= 1.
 */
#ifndef OMEGA_MATVEC_QUAD_H
#define OMEGA_MATVEC_QUAD_H

#include "omega_matvec.h"

/* Kind code carried beside the M12 kinds. Not a member of the M12 family. */
#define OMEGA_MATVEC_KIND_QUAD4 3u

int omega_matvec_synthesize_quad4(const MatVecSemanticSpec *spec, const OmegaMachineGraph *mg,
                                  MatVecRealization *out);

#endif /* OMEGA_MATVEC_QUAD_H */
