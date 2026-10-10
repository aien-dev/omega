# AT-1 real-engine physics mutants, post-freeze follow-up (G8 review item 3)

Run of `make at1-check` on omega `3f75bac8` (Spark, 20261010T062838Z), which adds the remaining AT1_SPEC 9.2
physics mutants to the runner: M2 flip V, M4 phase sign, M5 Y sign, M6 axis swap, M8 ideal marginal,
each applied to the real engine source by sed and compared cell by cell with Agent 4's predictions in
`evaluator/results/MUTANT_RECEIPT.tsv` (written before this run). M1 drop V and M3 wrong level rerun
unchanged. This directory is evidence for that follow-up only; the frozen AT-1 record (`ff81466`,
`evidence/AT1/20261010T044344Z-18e1786`) is not moved.

| mutant | killed | blind |
|---|---|---|
| M1 drop_v | 15/18 | P1a, P1b, P1c |
| M3 wrong_level | 16/18 | P1a, P1b |
| M2 flip_v | 15/18 | P1a, P1b, P1c |
| M4 phase_sign | 16/18 | P6, N1f |
| M5 y_sign | 16/18 | P6, N1f |
| M6 axis_swap | 18/18 | none |
| M8 ideal_marginal | 15/18 | P1a, P1b, P1c |

Every cell equals the predicted KILLED/BLIND. Gates G1 to G6 PASS on the same run, controls failing 0.
Files: `mutant-<name>.tsv` (case, predicted, got, how), `controls.tsv`, `toolchain.txt`, `provenance.static`.
PASS means spec conformance of the mutant detection only.
