/* c2_lrat: checker2's own DIMACS CNF reader and LRAT proof checker (omega#446).
 *
 * Trusted part of checker2. The SAT solver that writes proofs is NOT trusted and is not linked here.
 * Written from the published formats only: DIMACS CNF ("p cnf V C", clauses of nonzero literals
 * ended by 0, "c" comment lines) and textual LRAT (Cruz-Filipe, Heule, Hunt, Kaufmann, Schneider-Kamp,
 * "Efficient certified RAT verification", CADE 2017):
 *     <id> <lit>* 0 <hint>* 0        add a clause, justified by reverse unit propagation (RUP)
 *     <id> d <id>* 0                 delete clauses
 * checker2 accepts RUP steps only: a negative hint (a RAT step) is refused, never interpreted.
 * Each added clause is checked by assigning all its literals false and walking the hints in order:
 * every hint must be unit (then its last literal is assigned) or falsified (then the step is
 * proved). A hint that is neither, or a hint list that ends without a conflict, fails the proof.
 * The proof is accepted only if it adds the empty clause. Fails closed on any malformed input. */
#ifndef C2_LRAT_H
#define C2_LRAT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int nvars;
    int nclauses;
    int32_t *lits;      /* all clause literals, each clause ended by 0 */
    size_t nlits;
    size_t *start;      /* nclauses offsets into lits */
    size_t lcap, scap;  /* allocated entries of lits / start */
} C2Cnf;

typedef struct {
    int ok;             /* 1: the empty clause was derived, the formula is unsatisfiable */
    const char *rule;   /* stable failure name, "LRAT-OK" on success */
    long line;          /* proof line of the failure, 0 if none */
    long steps, deletions;
} C2LratResult;

/* 0 ok, -1 malformed (rule in *why). Caller frees with c2_cnf_free. */
int c2_cnf_parse(const char *text, size_t len, C2Cnf *cnf, const char **why);
void c2_cnf_free(C2Cnf *cnf);
/* Append clause (lits[0..n), 0 excluded) to an in-memory formula (used by the bit-blaster). */
int c2_cnf_add(C2Cnf *cnf, const int32_t *lits, int n);
/* DIMACS text of cnf into a malloc'd buffer. */
char *c2_cnf_print(const C2Cnf *cnf, size_t *len);

/* 0 proof accepted (empty clause derived), -1 refused (r->rule says why). */
int c2_lrat_check(const C2Cnf *cnf, const char *proof, size_t len, C2LratResult *r);

#endif /* C2_LRAT_H */
