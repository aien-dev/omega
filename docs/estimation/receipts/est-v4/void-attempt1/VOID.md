# ESTIMATION v4, attempt 1: VOID (external load during both collections)

This folder holds the first complete v4 attempt, moved here unchanged (git history
keeps the original paths). It is **void** and is not the v4 verdict.

## Why

After the sealed run, the coordinator reported that another lane ignored the
machine flag:

- host build and test suites (native/store, multi-core) at about 09:12-09:21 UTC and a
  short test near 09:30 UTC, inside the D1 window 08:45:08-09:30:08 UTC;
- a QEMU run started at 09:33:30 UTC that overlapped the D2 window from 09:35:59 UTC.

Both collections were therefore not taken under the declared load schedule alone.
The coordinator directed that samples from those windows be treated as invalid,
discarded and recollected. The section 7 pre-check had called both VALID (D1
foreign mean 1.2555 busy cores, close to the 1.5 limit; D2 0.5929), so the
pre-check alone did not catch it.

## What the void attempt showed (for the record only)

Phase A PASS (G1 selected); sealed run PASS, receipt
`receipt-acb6fc1ea4ced9350a4b72e1f53767c05bec776a2543ad595512da9f19329227.json`.
The void attempt passed, so voiding it cannot make v4 easier to pass; it can only
replace a contaminated PASS with a clean PASS or a clean FAIL.

## What happens instead

The whole order of operations (protocol section 8, steps 2-6) is rerun once on fresh
data with the protocol's recollection seeds (section 7): D1 seed 0xE5C4D1 + 1 =
0xE5C4D2, D2 seed 0xD2E5C4 + 1 = 0xD2E5C5. Same frozen protocol, same tool code, no
change to families, grids, bands or rules. The rerun's verdict is the v4 verdict,
whatever it is. During the rerun a process monitor logs any other heavy process;
a collection that overlaps foreign heavy work is void again and is recollected
(seed + 2), and a third bad collection ends v4 as INCONCLUSIVE.

Raw folders of this attempt carry an `INVALID` note and are never used again:
`evidence/EST4/raw/20261001T084508Z-est4-fit-silicon/`,
`evidence/EST4/raw/20261001T093559Z-est4-heldout-silicon/`.
