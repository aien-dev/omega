# DIRAC-0 independent oracle specification (PR D-02, evaluator side only)

Status: NOT_RUN. Nothing in this directory is built, compiled or executed. The machine quiet flag was up when this was written.
Authority: `aien-architecture` `docs/plans/dirac/DIRAC-0-SPEC.md` (PROPOSED) and Drake's DIRAC-0 brief of 2026-10-01. The brief wins on any difference.

## 1. What the oracle is

The oracle is the evaluator's own, separate way to know the right answer for the Dirac conformance fixture. It checks an Omega candidate. It is never checked by it, never imported by it, and never needed to build it.

Hard properties:
1. Shares no source, header, build rule or test harness with any Omega implementation. It does not include `src/` headers, does not link `omega_numeric` or the M20 tensor layer, and has no Makefile hook.
2. Is never an Omega dependency. Removing this directory must leave every Omega build and test unchanged.
3. Dirac is not an Omega primitive. This document proposes no Dirac-named builtin (no DiracSpinor, GammaMatrix, Electron, Mass, Spin). Omega-side work in D-03 to D-06 is generic (complex scalar, operator relations, constraint-defined algebras, differential operators) and justifies itself without this oracle.
4. Evaluator world only. Gamma matrices, the equation, interpretation and answer-key mappings live here and nowhere on the AIEN side. See section 8.
5. No libm, no floating point for reference values. Plain C and POSIX sh, integer arithmetic only. No outside dependencies. No Python.

## 2. Capabilities (evaluator capabilities, not AIEN facts)

### 2.1 Gamma representations
Declared per record: dimension `d` (number of generators, time plus space), matrix size `n`, metric diagonal, and a representation name.
- 1+1D first: n = 2, generators g0, g1, metric diag(+1,-1) (signature declared, never assumed).
- Representations: Dirac (g0 diagonal), Weyl/chiral (g0 off diagonal), Majorana (all entries real or purely imaginary as the signature requires).
- 3+1D later (Wave 4 step D1C-7): n = 4, g0..g3, metric diag(+1,-1,-1,-1).
- Matrix entries are Gaussian rationals (a/b + (c/d)i). No floating text.

### 2.2 Clifford relation check
For every pair i,j: g_i g_j + g_j g_i = 2 metric(i,j) I, exactly, in Gaussian rational arithmetic. Result is a list of failing pairs. Zero failures is PASS, anything else FAIL. No tolerance: the check is algebraic.

### 2.3 Analytic plane-wave cases
For declared rest parameter m and momentum p (rationals), the oracle states the exact energy where it is rational and a rational interval otherwise. Spinor amplitudes are given up to the declared normalization. The corpus carries the case, the representation, and the expected values.

### 2.4 Dispersion cases
Relation E^2 = p^2 + m^2 (units declared in the record). Cases are chosen so E is rational where possible (for example m = 1, p = 3/4 gives E = 5/4) and otherwise carry a high precision value with a declared bound.

### 2.5 Basis-change equivalence
Two representations A and B are equivalent if an invertible S exists with S g_i^A S^-1 = g_i^B for all i. The oracle checks the supplied S and S^-1 exactly (S S^-1 = I and the conjugation identity), then checks that declared observables (here: the dispersion energy and the trace of products of generators) agree between the two. Observables, not matrices, are what must agree.

### 2.6 High precision references
Arbitrary precision implemented in this directory using only integer arithmetic (a simple in-file big integer over base 10^9 limbs, or exact rationals, no outside code). Every value is declared as `value = N / 10^k` with an integer bound `B` meaning |true - value| <= B / 10^k. A reference value is accepted into the corpus only when two independent routes agree within the declared bound:
- Route A: direct evaluation of the closed form (for example an integer square root by Newton iteration on the scaled integer).
- Route B: bracketing by bisection on exact rational squares.
Disagreement beyond the bound is a corpus defect, recorded, not patched around.

### 2.7 Wave 4 ladder (capabilities added in this order, each its own PR, none are AIEN facts)
D1C-1 momenta, D1C-2 rest parameters, D1C-3 superpositions, D1C-4 wave packets, D1C-5 external fields, D1C-6 higher dimensions, D1C-7 full 3+1 Clifford.

## 3. Tolerance policy
- Algebraic checks (Clifford, basis change, exact dispersion): exact, no tolerance.
- Numeric candidate outputs compared with oracle values: integer bounds, named as FORGE names them, `error_rel_ppb` (relative, parts per billion) and `error_abs` (absolute, in declared units of the last place), with the FORGE error kind (EXACT, BOUNDED_DETERMINISTIC, BOUNDED_STOCHASTIC, MEASURED_DISTRIBUTION) declared per case. Bit-level comparisons on the E1 side use the existing compare vocabulary (OMEGA_CMP_BIT_EXACT, OMEGA_CMP_INT_EXACT) unchanged.
- A bound is declared before the candidate runs and never widened after seeing a result.

## 4. Mutation test plan (written, not run)
The oracle must reject broken fixtures. A scratch copy of the corpus is mutated and the verifier must fail with a named check:
1. Flip one sign in g1 (so g1^2 = +I under metric -1): CLIFFORD_CHECK must fail.
2. Swap g0 and g1 without swapping metric entries: CLIFFORD_CHECK must fail.
3. Drop the factor 2 in the anticommutator target: must fail.
4. Perturb S^-1 by one entry: BASIS_CHANGE must fail.
5. Change one dispersion digit beyond its bound: DISPERSION must fail.
6. Change one byte in the corpus body: digest check must fail.
Each mutation is a documented patch, applied to a scratch copy only.

## 5. Independence checklist (reviewer ticks each before D-02 merges)
- [ ] No `#include` of anything under Omega `src/`.
- [ ] No shared constants or tables copied by reference (copy by value is allowed only for published textbook representations, and is cited).
- [ ] No Makefile, mk/ or test-runner hook.
- [ ] Reference values produced by two routes (section 2.6).
- [ ] Authors of any future Omega implementation do not edit this directory (lane B is independent of lane A).
- [ ] No Python files; no libm.

## 6. Corpus format
See `CORPUS-FORMAT.md`.

## 7. Source in this directory (all NOT_RUN)
`src/oracle_clifford.c` (exact Clifford relation check), `src/oracle_kat_verify.sh` (digest check), `README-NOT-BUILT.md`.

## 8. Keeping the corpus out of AIEN reach
- The corpus and oracle live only in this evaluator-only directory. They are never copied into Omega `src/`, `tests/`, `evidence/`, `docs/`, any Cortex store, prompt, Skill or graph.
- Filenames, directory names and metadata that could reveal the answer (words such as Dirac, gamma, spinor, Clifford) must not appear on anything AIEN can read. The AIEN side receives only numeric observations, channel ids, timestamps, permitted interventions and uncertainty, produced later by the sealed dataset generator (D-11) from evaluator inputs.
- Sealing uses the existing G3 sealed-holdout commitment scheme (`spec/searchtrace/G3_SEALED_HOLDOUT_COMMITMENT_V1.md`); real sealing is BLOCKED_OPERATOR today.
- Note for reviewers: this directory is in the omega repository, which AIEN could read. Therefore everything here is the PUBLIC conformance corpus (textbook representations and relations that reveal nothing about a hidden experiment). Sealed experiment parameters and answer keys never go in this repository; they belong in the private sealed store owned by the operator.
