# EST-3b protocol v2: recorded outcome (2026-10-01)

Protocol: `docs/estimation/EST23_PROTOCOL_V2.md` (frozen 3ba6be1, SHA-256
86b7b46f...36f5). Tools: 6c1e712 + digests pinned in 2ce4fb7 (clean tree).

```text
C1 PRE-CHECK  = FAIL
  run C1 evidence/EST3B/raw/20261001T020412Z-est3b-fit-silicon (SHA-256 65252bae...5716)
  zone 0 horizon-1 one-step changes at L >= 30: n = 2060, exactly zero = 1656
  exact-zero fraction 0.8039 > 0.54 (protocol v2 section 5 step 3)
ESTIMATION_CALIBRATION (v2) = FAIL, quantization class (v1 section 5a)
```

Recomputed twice: from `est_replay 2` innovations and directly from the raw
lines with awk; both give 1656 / 2060.

## What this means
- In the declared idle regime the sensor sits on the same 100 mC reading for
  about 80 % of seconds. No continuous predictive can put 50 % coverage inside
  [0.46, 0.54] when 80 % of outcomes are a single exact value, so M2 was not
  fitted, C2 was not collected and nothing was evaluated on held-out data.
- v1's failure (loaded regime, heavy tails) is still not repaired on its own
  regime. v2 never tested the loaded regime.
- Bands, burn-in and models were not changed. No estimator is promoted.

## What a v3 would need (not started)
Either a declared loaded regime with load-trial marks (needs a scheduled heavy
run owned by another session), or a protocol whose interval rule handles a
discrete outcome (for example randomized PIT coverage), entered under the
quantization failure class.
