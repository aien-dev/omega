# CAND-3 window W2: claim index (added after the window; nothing else in this directory changed)

Window W2, quietlock hold q57951-1791251173-2972b958, 2026-10-06T01:46:13Z to 02:09:58Z. Declared before it ran:
`00-declared/DECLARED-ATTEMPTS.md` (sha256 9f4b8b91...f184) and `00-declared/DECLARED-ATTEMPTS-w2.md` (ef42d9c8...2ba1).
Candidate: CAND-3 (aien-architecture 951cade); omega code f816473, harness 97ee275 (map-only descendant, guard checked
at every lane start and end), physics 6d7cf0d. W1 (evidence-out/CAND3-LIVING-f816473) is INVALID and kept separately.
Digests below are sha256 prefixes of the files named, in this directory.

| # | Claim | Result | Evidence |
|---|---|---|---|
| A1 | R16 ladder | NOT_RUN overall (declared): G1-G6 PASS, G7 NOT_RUN (no R15 acceptance receipt), G8 NOT_RUN (merge-commit step) | `R16-ladder/stdout.log` (197a641eb62788f2) "Gates: G1=PASS ... G6=PASS G7=NOT_RUN G8=NOT_RUN", "R16_QUALIFY_RESULT=NOT_RUN"; exit 3; receipt `R16-ladder/raw/receipts/95ce06cf...json` |
| A1/G6 | operator emergency control by execution | PASS: host, mutants and silicon in the same run; "G6=PASS (items missing: 0)" | host receipt `raw/.../operator_host_receipt.json` (ed8d7d5b, binary 605054a7); silicon receipt `operator_silicon_receipt.json` (b617f180, binary 9f48df86); `r16_operator_mutants.log` (3b94e8ef4a6c2029) "25 mutants, 0 failures, 0 skipped", "RX_OPERATOR_MUTANTS: PASS"; both binaries MATCH the double build (`R16-ladder/executables-vs-double-build.txt`) |
| A2 | R13 test build on silicon, candidate bound | PASS | `R13-testbuild-silicon/stdout.log` "R13_LIVING_SYSTEM_TEST_BUILD=PASS"; receipt `qual-runs-after-R13/20261006T020106Z-97ee27584cda/R13/rx_living_test_build_receipt.json` (638b19f6), candidate_bound true, candidate_commit 97ee275, tree_dirty false; binary MATCH |
| A3 | production hygiene on silicon | PASS | `prod-hygiene-silicon/stdout.log` (eeaecad7970bf082) "PROD_HYGIENE=PASS mode=silicon"; exit 0 (0 s; CAND-2's took 0 s too) |
| A4 | COMPOSITION-2 on the GPU | PASS | receipt `COMPOSITION-2-GPU/receipt-0eda1fec...json` verdict PASS; stdout "failures=0 ... chip_ok=1"; binary MATCH |
| A1b | R11 living under load, living run exercised | PASS | `R11-living/stdout.log` (a1c1e88dff773b4f): exit 0, "checks 436 failures 0", "[*] living run: moved from Cortex-A725 to Cortex-X925"; no "living run not exercised"; 1-minute load 1.01 at start (`R11-living/machine-before/uptime.txt`) |
| A5 | M19 / CHIPWAIT campaign, 3/3 rule | PASS: 3 usable runs, 0 invalid, 0 failed | `CHIPWAIT/campaign/campaign.json` (22fecfd0d2841968) verdict PASS; per-run `run-00N/verdict.json`. Limit: each soak is 100,000 cycles in about 41-43 s, not hours of endurance |
| A6 | M18 matmul gates | PASS: 18/18 M18 gates, 36 evaluated | `M18/stdout.log` (7317ef22eed3b702); omegatool MATCH the double build |
| A7 | R15 silicon performance | NOT_RUN: not attempted (declared); energy reader not loaded | no lane run; `00-declared/window.txt` lists no r15 lane |
| A8 | correctness reruns for carried results | none needed (declared): carried results rest on IDENTICAL executables | CAND-3.gates.md section 2 |
| - | machine condition | Xid 0 before and after every step | `*/machine-before/xid-count.txt`, `*/machine-after/xid-count.txt` |

## Limits and findings (recorded, they change no result)

1. **Mutant receipts beside real ones.** `qual-runs-after-R16/` and `qual-runs-after-R13/` hold R13 receipts `20261006T015956Z` and
   `20261006T020018Z` whose gate reads FAIL (the second also "argus": FAIL, authority_observations 0). They were written
   between 01:55:36Z and 02:00:44Z by `tests/runtime/rx_operator_mutants.sh`: each mutated copy links `build` to the real
   build directory (line 48, `ln -s "$HERE/build" "$1/build"`), so its receipt lands in `build/qual-runs`. Their binary digests
   (4259b1f7..., 1c612eef...) are not the production binaries (605054a7..., 9f48df86...). They are mutants the test killed, as
   required; they are not results of the candidate. Follow-up: mutant runs need their own receipt directory.
2. **R11 receipt file not kept.** The R11 lane does not copy `build/qual-runs`; the CHIPWAIT lane's `make clean` then removed
   `build/qual-runs/20261006T020212Z-97ee27584cda`. The declared pass rule reads the exit code and stdout, which are kept. Same as CAND-2.
3. **Not claimed:** R15, R16 G7, R16 G8, R16 as a whole; physical boot, owner-key ceremony, firmware, TPM, real storage device; any
   hardware TRUST qualification.
