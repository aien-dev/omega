# M20 fixture table (not_run: GB10 parity PASS + one NOT_RUN row)

## Qualification checklist (plan E2)

| Item | Status | Evidence |
|---|---|---|
| Shape / type checks | PASS (CPU) | `test-tensor` |
| CPU parity | PASS (CPU) | `test-tensor` |
| Mutation / refusal | PASS (CPU) | `test-tensor-mutations` |
| CI | WIRED | hosted runner, not a qualification |
| E1 numeric closure | PASS (on main) | E1 receipt |
| GB10 parity | PASS (GB10) | chip evidence sha256 70a987ec6835d37836d0cc415e8b609df6edda6cd560a0b86e238b24cf4bd6d8 |
| Reproducible receipt | NOT_RUN | awaiting forge |

**M20 verdict: fixture.**

## Open seams

| Not | NOT_RUN | rows after the verdict are ignored |
