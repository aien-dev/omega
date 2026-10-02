# Gate 14 refresh on omega 1720a8d + physics e95e3ed (PR #225 candidate)

Status: **M19 is incomplete.** Accuracy and build/tooling passed on GB10. The endurance gate did not execute its 1,000-job workload because it exited before submitting the first GPU job. This does not qualify or disqualify #225. The endurance gate will be rerun against current `main` after the E1 campaign to distinguish a harness/machine failure from a branch-specific regression.

| Item | Status |
|---|---|
| Host checks (combined E1 branch) | pending |
| M19 accuracy (Gate 5) | PASS |
| M19 build/tooling (Gates 3/4) | PASS |
| M19 endurance (Gates 1/2 sustained loop) | NOT ESTABLISHED (workload not executed) |
| M19 overall | NOT QUALIFIED |

Result: **no combined M19R foundation receipt was produced.** Two of the three legs have PASS receipts. The Gates 1/2 qualifier exited 1 (recorded verbatim below); its endurance workload never ran.

| Leg | Verdict | Receipt |
|---|---|---|
| Gate 5 numeric (chip) | PASS | `evidence/OMEGA-NUMERIC-0/3bb806d2605d2cc07de539c535c0777ec4328bc4e57f0336d817afab64e5bab8.json` |
| Gates 3/4 forge (chip) | PASS | `evidence/FORGE-GATES/69bdfdf3ea9f42a0aa456d165105271bf2b5f91b16160a0d6c9fe5b06bdbb115.json` |
| Gates 1/2 M19R qualification (chip, full soak mode) | qualifier exit 1; endurance NOT ESTABLISHED | none written by the qualifier; raw run in `m19r-run/` |

Next (in order): E1 chip campaign; then the endurance leg alone on current `main`. If `main` also submits zero jobs, investigate harness/device/machine. If `main` passes, rerun the same leg on the #225 combined branch; #225 becomes a suspect only if `main` passes and #225 fails. R1-R15 stays frozen until this is resolved.

The Gates 1/2 run (2026-10-02 17:34:34Z to 17:51:06Z, about 16.5 minutes) passed the physics suites, the world lifecycle suite and 157 of 157 prior regression gates, but the 1000-operation sustained loop in the M19 suite returned failure (`sustained_dispatches` 0). That failed 1000_OP, MEMORY_BOUND, STATE_DIGEST, CLEAN_CLONE and RECEIPT, so the M19 suite reported 13 of 18 and `omegatool` exited 1.

Cause is UNVERIFIED (candidate or machine). The previous combined receipt `50dd611b...` (omega 8024e9a) passed this same leg. The #225 diff does not touch the world gate code. A control run on a06f484 would separate the two; it was not run (chip reserved for other work). The FAIL evidence is kept as is. Kernel log: plain `dmesg` is refused for this user (`m19r.dmesg.txt` holds only that error). After the run it was read with `sudo -n dmesg` (`dmesg-after-run-sudo.txt`, taken 17:55Z): no Xid lines. The three test binaries of the run are not stored; their sha256 are in `m19r-run/test-binaries.sha256`.

`tools/gate14_combine.sh` requires all three legs on one pair, so it was not run. The earlier combined receipt on 8024e9a is unchanged.
