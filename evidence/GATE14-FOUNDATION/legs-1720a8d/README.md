# Gate 14 refresh on omega 1720a8d + physics e95e3ed (PR #225 candidate)

Result: **no combined M19R foundation receipt was produced.** Two of the three legs have PASS receipts; the third (Gates 1/2) FAILED.

| Leg | Verdict | Receipt |
|---|---|---|
| Gate 5 numeric (chip) | PASS | `evidence/OMEGA-NUMERIC-0/3bb806d2605d2cc07de539c535c0777ec4328bc4e57f0336d817afab64e5bab8.json` |
| Gates 3/4 forge (chip) | PASS | `evidence/FORGE-GATES/69bdfdf3ea9f42a0aa456d165105271bf2b5f91b16160a0d6c9fe5b06bdbb115.json` |
| Gates 1/2 M19R qualification (chip, full soak mode) | FAIL | none written by the qualifier; raw run in `m19r-run/` |

The Gates 1/2 run (2026-10-02 17:34:34Z to 17:51:06Z, about 16.5 minutes) passed the physics suites, the world lifecycle suite and 157 of 157 prior regression gates, but the 1000-operation sustained loop in the M19 suite returned failure (`sustained_dispatches` 0). That failed 1000_OP, MEMORY_BOUND, STATE_DIGEST, CLEAN_CLONE and RECEIPT, so the M19 suite reported 13 of 18 and `omegatool` exited 1.

Cause is UNVERIFIED (candidate or machine). The previous combined receipt `50dd611b...` (omega 8024e9a) passed this same leg. The #225 diff does not touch the world gate code. A control run on a06f484 would separate the two; it was not run (chip reserved for other work). The FAIL evidence is kept as is. Kernel log access: `dmesg` as the normal user is refused, so `m19r.dmesg.txt` holds only that error; no Xid check was possible for this run.

`tools/gate14_combine.sh` requires all three legs on one pair, so it was not run. The earlier combined receipt on 8024e9a is unchanged.
