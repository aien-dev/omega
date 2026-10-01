# Lane 32 item 4: R12-R16 re-qualification on omega 3108fc2f5d499a76d6ed72f53681c3e2ba9281a4

Run by lane32 worker D on the Spark, 2026-10-01, one gate at a time under the quiet flag, on a clean worktree
pinned to the candidate (git status --porcelain empty before and after every gate). Every receipt below records
candidate_commit = 3108fc2f5d499a76d6ed72f53681c3e2ba9281a4, candidate_bound true, tree_dirty false.
Verdicts come from the raw gate lines (gate_lines/59b762f4...txt), not from tools/r16_qualify.sh.

| Gate | Verdict | Receipt (sha256 file name) |
|---|---|---|
| R12 silicon (test-r12-silicon) | PASS | R12_silicon/3f1c6ca5d86f7636d4bd03ba37a73bb7e650040b7ca253f8cc81ee55b25caeb9.json |
| R12 host (test-r12) | HOST_RULES_PASS | R12_host/2a41244f01a909bda801bbaa1d431b1e298f678b53772211552347440541797e.json |
| R13 production silicon (test-r13-silicon) | PASS | R13_prod_silicon/abe1feb3134c59cad36bc5edb904653c52c610d961e8a3cd41d1c996f058f8a5.json |
| R13 production host (test-r13-host) | HOST_PASS_NON_SILICON | R13_prod_host/4af725e4418942ff65f09339884edd1b01119dc0273dc45909eb9bbe0d40edc3.json |
| R13 test-build silicon | PASS | R13_testbuild_silicon/1912d5c5eacf38e0ba3fef1debb8140365a8288fdaf2487b964628f84d6d22bc.json |
| R13 test-build host | HOST_PASS_NON_SILICON | R13_testbuild_host/a6104944fe231f14ea5d723c8cfd808cfb32bab1f15afded8b475ff302bd82d1.json |
| R14 silicon | PASS | R14_silicon/94693d5cfd60ee3e04e2e78ba3773b24ecf4d2d63d665ecb43e5bfb46c23ea90.json |
| R14 host | HOST_PASS_NON_SILICON | R14_host/ee936bed8fcb2947b4bc0354cfe5b8df025a051c610e7a359f04d3137ca862e9.json |
| R15 performance qualification (silicon) | FAIL (14 of 16 gates; G1, G15) | ../R15/b4acabd2b73d20a81cf87ecf6fdc4db9b428b9793063c0da64b5c27dc9b2212f.json |
| R15 parity host and silicon, G7 host, instrumentation, receipt test | PASS | gate lines only (no receipt file) |
| R16-G3 authpath silicon | PASS | R16_G3_authpath_silicon_R13/647c1fa0db8cda533c3333c075f57a667fd9792005bf8ce438ce6817d2807116.json and _R14/5239b1fc81cebf60d5cb2577b974dfa526256b846079893db4186994a3f592b0.json |
| R16-G3 authpath host | HOST_PASS_NON_SILICON | R16_G3_authpath_host_R13/83491424706a8cd5ec24c6501ea78a01ff6826038455c72b78035eda2c585339.json and _R14/65db1455b81cbef3388b1fcd1579fcdb3424091cb9072e8e418020e238fc93ef.json |
| R16-G1/G2 loop inventory | FAIL (47 unclassified sites) | R16_summary/ff9d8801990f530e560db2104a491bced1c7c030d0b3c63d8d0231c2cc6cc638.json |
| R16-G4 negative + 36 mutants | PASS | R16_summary |
| R16-G5 surface | PASS | R16_summary |
| R16 overall | FAIL | R16_summary/ff9d8801990f530e560db2104a491bced1c7c030d0b3c63d8d0231c2cc6cc638.json |
| test-prod-hygiene-silicon, test-prod-refuses-test-pieces | PASS | gate lines only |
| R3, R7, R8, R9, R10, R11 host ladder | PASS (R11 receipt says HOST_PASS_LIVING_RUN_NOT_EXERCISED) | ladder_R3 .. ladder_R11 |

R15 FAIL: trial RES-4 round 12 did not complete (the graphics seat did not finish). The kernel logged
Xid 109 CTX SWITCH TIMEOUT from rx_r15_perf_silicon 3 s into that trial (14:40:47Z). Not rerun. A prior Xid 13 from
Lane 2 test_omega_numeric (13:15:48Z) is in the same boot; possible factor only.
Full R15 raw directory (25 MB) kept at ~/handoffs/lane32/D/r15raw-full; an excerpt is in R15_raw_excerpt/.
R16-G1/G2: the retirement map is stale against code added since its last update; 47 unclassified sites listed in the gate lines.
