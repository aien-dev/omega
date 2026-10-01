# ESTIMATION v4, attempt 2: VOID (foreign host builds and tests during both collections)

This folder holds the second complete v4 attempt (recollection seeds 0xE5C4D2 and
0xD2E5C5), moved here unchanged. It is **void** and is not the v4 verdict.

## Why

After the sealed run, the coordinator relayed a further report: a worker ran a
roughly 2-minute host build at about 11:36-11:38 UTC, inside the D2 window
(11:28:13-12:13:15 UTC). The coordinator directed that samples from such windows be
excluded or recollected. Excluding samples would need a scoring change after the
freeze, which section 9 forbids, so the data is recollected instead.

The lane15 process watcher (`watch-d1.log`, `watch-d2.log`: top every ~5 s, foreign
processes above 25 % CPU, plus compiler/test process counts) shows the same kind of
foreign host work in **both** windows:

- D1 window (10:31:23-11:16:23 UTC): `make`/`cc1` at 10:36-10:38, 10:56-11:11 and
  11:16; single-core `test_osc_*` and `test_legacy_*` processes from 10:36 to 11:11.
- D2 window: `make`/`gcc`/`cc1` at 11:28-11:32 and 11:38; a single-core process `t`
  from 11:28 to 11:32.

The watcher never saw two foreign processes above 25 % at once, and the section 7
pre-check called both VALID (foreign mean 0.5084 and 0.3737 busy cores). Attempt 2
had been judged valid on that basis. Under the coordinator's stricter standard (no
foreign host build or test at all during a collection window) it is not, so it is
voided as a whole, D1 included, for consistency.

## What the void attempt showed (for the record only)

Phase A PASS (G1 selected, params sha256 `c5899de7...b80b6`); sealed run PASS, receipt
`receipt-20da3468eff2eac21dc1d9eb2ecca1505ad1961315579f24ffdec488609bc2d9.json`
(G1 held out: cov95 0.9570, ten-step cov95 0.9534). The void attempt passed, so voiding
it can only replace a PASS with a clean PASS or a clean FAIL.

## What happens instead

Attempt 3 reruns protocol section 8 steps 2-6 with the last recollection seeds the
protocol allows (section 7, n = 2): D1 0xE5C4D3, D2 0xD2E5C6. Same frozen protocol and
tool code. Validity standard, fixed now, before any attempt-3 data: section 7 pre-check
VALID **and** no foreign compiler, test, QEMU or build process in the watcher log
during the window **and** no slip reported by the coordinator. If an attempt-3
collection fails that standard, there is no seed left, and v4 ends **INCONCLUSIVE**
(section 7: a third bad collection ends v4 as INCONCLUSIVE).

Raw folders of this attempt carry an `INVALID` note and are never used again:
`evidence/EST4/raw/20261001T103123Z-est4-fit-silicon/`,
`evidence/EST4/raw/20261001T112813Z-est4-heldout-silicon/`.
