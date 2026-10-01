# M20 fixture table (missing)

## Qualification checklist (plan E2)

| Item | Status | Evidence |
|---|---|---|
| Shape / type checks | PASS (CPU) | `test-tensor` |
| CPU parity | PASS (CPU) | `test-tensor` |
| Mutation / refusal | PASS (CPU) | `test-tensor-mutations` |
| CI | WIRED | hosted runner, not a qualification |
| E1 numeric closure | PASS (on main) | E1 receipt |
| Crash-safe storage lifetime | MISSING_IMPLEMENTATION | |

**M20 verdict: fixture.**

## Open seams

| Not | NOT_RUN | rows after the verdict are ignored |
