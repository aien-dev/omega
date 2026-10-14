/* test_c2_lrat: checker2's DIMACS reader and LRAT checker on hand-written cases (omega#446).
 * Every refusal is checked by its exact rule name, so removing any GUARD line in c2_lrat.c fails
 * a case here (tests/checker2/mutants.sh). Also: 20000 single-byte corruptions of a valid proof
 * never crash and never make a satisfiable formula "unsatisfiable". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c2_lrat.h"

static int fails, cases;

/* (1 2)(1 -2)(-1 2)(-1 -2): unsatisfiable */
static const char *UNSAT4 = "c four clauses over two variables\np cnf 2 4\n1 2 0\n1 -2 0\n-1 2 0\n-1 -2 0\n";
static const char *GOOD = "5 1 0 1 2 0\n6 0 5 3 4 0\n";

static void lrat(const char *name, const char *cnf_text, const char *proof, const char *want_rule)
{
    cases++;
    C2Cnf cnf;
    const char *why = "";
    if (c2_cnf_parse(cnf_text, strlen(cnf_text), &cnf, &why)) {
        printf("FAIL %s: cnf refused (%s)\n", name, why);
        fails++;
        return;
    }
    C2LratResult r;
    int rc = c2_lrat_check(&cnf, proof, strlen(proof), &r);
    int want_ok = strcmp(want_rule, "LRAT-OK") == 0;
    if ((rc == 0) != want_ok || r.ok != want_ok || strcmp(r.rule, want_rule) != 0) {
        printf("FAIL %s: rc=%d ok=%d rule=%s line=%ld, want %s\n", name, rc, r.ok, r.rule, r.line, want_rule);
        fails++;
    }
    c2_cnf_free(&cnf);
}

static void cnf(const char *name, const char *text, const char *want)
{
    cases++;
    C2Cnf c;
    const char *why = "CNF-OK";
    int rc = c2_cnf_parse(text, strlen(text), &c, &why);
    if (rc == 0) c2_cnf_free(&c);
    if ((rc == 0) != (strcmp(want, "CNF-OK") == 0) || strcmp(why, want) != 0) {
        printf("FAIL %s: rc=%d why=%s, want %s\n", name, rc, why, want);
        fails++;
    }
}

int main(void)
{
    /* accepted */
    lrat("valid", UNSAT4, GOOD, "LRAT-OK");
    lrat("valid-with-deletions", UNSAT4, "5 1 0 1 2 0\n5 d 1 2 0\n6 0 5 3 4 0\n", "LRAT-OK");
    lrat("valid-comments", UNSAT4, "c proof\n5 1 0 1 2 0\n\nc next\n6 0 5 3 4 0\n", "LRAT-OK");
    lrat("valid-trailing-hints", UNSAT4, "5 1 0 1 2 3 0\n6 0 5 3 4 0\n", "LRAT-OK");
    lrat("valid-tautology-step", UNSAT4, "5 1 -1 0 0\n6 1 0 1 2 0\n7 0 6 3 4 0\n", "LRAT-OK");
    lrat("valid-empty-input-clause", "p cnf 1 2\n1 0\n0\n", "3 0 2 0\n", "LRAT-OK");
    /* refused, one per rule */
    lrat("hint-satisfied", UNSAT4, "5 1 0 3 0\n", "LRAT-HINT-NOT-UNIT");
    lrat("hint-two-free", UNSAT4, "5 0 1 0\n", "LRAT-HINT-NOT-UNIT");
    lrat("hint-unknown", UNSAT4, "5 1 0 1 9 0\n", "LRAT-HINT-UNKNOWN");
    lrat("hint-deleted", UNSAT4, "4 d 2 0\n5 1 0 1 2 0\n6 0 5 3 4 0\n", "LRAT-HINT-UNKNOWN");
    lrat("rat-hint", UNSAT4, "5 1 0 -1 2 0\n", "LRAT-RAT-UNSUPPORTED");
    lrat("no-conflict", UNSAT4, "5 1 0 1 0\n", "LRAT-NO-CONFLICT");
    lrat("no-hints", UNSAT4, "5 0 0\n", "LRAT-NO-CONFLICT");
    lrat("no-empty-clause", UNSAT4, "5 1 0 1 2 0\n", "LRAT-NO-EMPTY-CLAUSE");
    lrat("empty-proof", UNSAT4, "", "LRAT-NO-EMPTY-CLAUSE");
    lrat("id-repeat", UNSAT4, "5 1 0 1 2 0\n5 0 5 3 4 0\n", "LRAT-ID-ORDER");
    lrat("id-input-range", UNSAT4, "4 1 0 1 2 0\n", "LRAT-ID-ORDER");
    /* ids are capped at (input clauses + proof lines): a huge id must not size the clause table */
    lrat("id-huge", UNSAT4, "5 1 0 1 2 0\n99999999 0 5 3 4 0\n", "LRAT-ID-RANGE");
    lrat("id-huge-first", UNSAT4, "100000000 1 0 1 2 0\n", "LRAT-ID-RANGE");
    lrat("id-at-cap", UNSAT4, "5 1 0 1 2 0\n7 0 5 3 4 0\n", "LRAT-OK");
    lrat("id-past-cap", UNSAT4, "5 1 0 1 2 0\n8 0 5 3 4 0\n", "LRAT-ID-RANGE");
    lrat("literal-range", UNSAT4, "5 7 0 1 2 0\n", "LRAT-LITERAL");
    lrat("literal-range-neg", UNSAT4, "5 -3 0 1 2 0\n", "LRAT-LITERAL");
    lrat("truncated", UNSAT4, "5 1 0 1 2", "LRAT-SYNTAX");
    lrat("garbage", UNSAT4, "5 x 0\n", "LRAT-SYNTAX");
    lrat("id-zero", UNSAT4, "0 1 0 1 2 0\n", "LRAT-SYNTAX");
    lrat("delete-unknown", UNSAT4, "4 d 9 0\n5 1 0 1 2 0\n6 0 5 3 4 0\n", "LRAT-DELETE-UNKNOWN");
    lrat("delete-twice", UNSAT4, "4 d 1 1 0\n", "LRAT-DELETE-UNKNOWN");
    lrat("satisfiable-formula", "p cnf 2 2\n1 2 0\n-1 -2 0\n", "3 0 1 2 0\n", "LRAT-HINT-NOT-UNIT");
    lrat("conclusion-on-wrong-formula", "p cnf 2 3\n1 2 0\n1 -2 0\n-1 2 0\n", "4 1 0 1 2 0\n5 0 4 3 0\n",
         "LRAT-NO-CONFLICT");

    /* DIMACS reader */
    cnf("cnf-ok", UNSAT4, "CNF-OK");
    cnf("cnf-no-header", "1 2 0\n", "CNF-HEADER");
    cnf("cnf-bad-header", "p cnf x 2\n", "CNF-HEADER");
    cnf("cnf-count-high", "p cnf 2 3\n1 2 0\n", "CNF-COUNT");
    cnf("cnf-count-low", "p cnf 2 1\n1 2 0\n-1 0\n", "CNF-COUNT");
    cnf("cnf-unterminated", "p cnf 2 1\n1 2\n", "CNF-COUNT");
    cnf("cnf-literal-range", "p cnf 2 1\n1 3 0\n", "CNF-LITERAL");
    cnf("cnf-garbage", "p cnf 2 1\n1 y 0\n", "CNF-LITERAL");

    /* robustness: byte corruptions of a valid proof against a satisfiable twin never verify, and
     * against the unsatisfiable formula never crash */
    const char *sat2 = "p cnf 2 4\n1 2 0\n1 -2 0\n-1 2 0\n2 0\n";
    C2Cnf u, s;
    const char *why;
    if (c2_cnf_parse(UNSAT4, strlen(UNSAT4), &u, &why) || c2_cnf_parse(sat2, strlen(sat2), &s, &why)) return 2;
    char buf[64];
    size_t n = strlen(GOOD);
    unsigned long x = 12345;
    long sat_accepts = 0, runs = 0;
    for (int it = 0; it < 20000; it++) {
        memcpy(buf, GOOD, n + 1);
        x = x * 6364136223846793005UL + 1442695040888963407UL;
        size_t at = (x >> 33) % n;
        buf[at] = " 0123456789-d\nc"[(x >> 20) % 15];
        C2LratResult r;
        c2_lrat_check(&u, buf, n, &r);
        if (c2_lrat_check(&s, buf, n, &r) == 0) sat_accepts++;
        runs++;
    }
    cases++;
    if (sat_accepts) { printf("FAIL corruption: %ld proofs verified a satisfiable formula\n", sat_accepts); fails++; }
    c2_cnf_free(&u);
    c2_cnf_free(&s);

    printf("test_c2_lrat: %d cases, %ld corruptions, %d failures\n", cases, runs, fails);
    return fails ? 1 : 0;
}
