# BRW-ACT-DEV1: successor to BRW-ACT-DEV0 (development profile)

Status: DEVELOPMENT DEMONSTRATION PROFILE, successor of `BRW_ACT_DEV0_PROFILE.md` (sha256
7ef3ba429d554b45046b88f0782f9c6e12f1074d9ea7287822ba5a1688efbfd4). NOT EXP-003; same limits as DEV0 section 7.
Written from public material only, after seeing DEV0's development output (`evidence/BRW-ACT-DEV0/dev/`).
DEV0 stays as written: its development FAIL and its held-out result are kept under `evidence/BRW-ACT-DEV0/`.

## Why a successor (design defects found on DEV0 development worlds, not code defects)

1. Unreachable target for the `diffusion` class. The candidate's grids nest: M1 at |v| = 0.02 and M2 at theta = 0.01
   predict almost the same readings as M0, and those hypotheses cannot be excluded inside any practical budget. With the
   prior of DEV0 section 2, once only the right D survives, the posterior mass ratio is bounded near
   M0 : M1 : M2 = 1/48 : 2/768 : 1/768, so P(M0) stays near 0.84 or below. The 0.99 target can never be met for a
   pure-diffusion world by any strategy. In DEV0 dev, every strategy was censored on all 40 diffusion worlds; the best
   P(M0) in any active run was 0.74.
2. Budget too small for the `ou` class: 0 or 1 of 40 dev worlds reached 0.99 under any strategy, so the primary outcome
   was censored at the horizon for almost every run and could not separate strategies.
3. The information computation was checked independently (Monte Carlo, 200000 draws per tau, at the prior): library and
   Monte Carlo agree within Monte Carlo error for I(Y;M) and I(Y;H) at every tau. The chooser is not the cause.

## Changes from DEV0 (everything not listed here is identical: world formulas, grids, prior, choice rule, verdict,
## prediction output, strategies, statistics, pass thresholds, replay rules)

| item | DEV0 | DEV1 |
|---|---|---|
| `diffusion` class role | discoverable | control "nested null": the correct behaviour is not to claim M1 or M2 |
| discoverable classes for P1, P2, P3 | diffusion, drift, ou | drift, ou |
| P4 | noise worlds: active claims M1 or M2 in <= 5% | noise worlds AND diffusion worlds, each separately: active claims M1 or M2 in <= 5% |
| budget per run | 400 | B, fixed by the power rule below before any DEV1 held-out world is run |
| world stream tags | `dev`, `hold` | `dev1`, `hold1` (fresh worlds; DEV0's worlds are not reused) |
| world counts | 40 per class dev, 100 per class hold | unchanged |

Power rule for B (strategy-neutral, uses one baseline only): run the `cycle` strategy alone on the `dev1` drift and ou
worlds at budgets 1000, 2000 and 4000. B is the smallest of these at which `cycle` reaches the correct model at
P >= 0.99 in at least half of those 80 worlds; if none does, B = 4000 and the receipt says the primary outcome may still
be censored often. The active strategy is not run, and no other baseline is consulted, when B is chosen. The power
table is committed as evidence before the `hold1` run.

DEV1 passes only if P1 to P5 (as amended above) all hold on `hold1`. Further changes make DEV2.
