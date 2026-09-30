# EST-2/3 tools hostile review (worktree omega-est, commit 4b3d720, protocol f96dc97)
Read-only. Run A and synthetic data only. Scratch builds and harnesses in
<scratch> (b/, indep.c, mut/, edge/, trunc/).
Protocol doc unchanged since f96dc97 (git diff empty), worktree clean.

## Counts: BLOCKER 1, MAJOR 3, MINOR 9, NIT 5

## Findings

### F1 BLOCKER: the recorded run leaves no per-step stream, yet the receipt claims one
- est_eval.c:334-348, 374-376, 360; est_replay.c:399 (only est_replay writes the stream).
- Protocol s2: "The output stream records, for each step, the observation record, prediction record, innovation record, plus their digests"; s5 REAL_SIGNAL needs "chain recorded for every step". est_eval never writes or hashes a stream. Receipt only says `chain_recorded_every_step: true` (computed from an in-memory equality check). After the one recorded run of est_eval on B there is nothing to audit, and the only way to get the stream is a second tool execution on B (est_replay --recorded), which s6 says is a new protocol version ("Any second execution on run B").
- Fix: make est_eval write the M0 and M1 streams (est_replay_write_stream) next to the receipt and put their SHA-256 in the receipt, all inside the one recorded invocation. Or declare in writing, before running, that est_replay --recorded on B is part of the single recorded execution and cross-check its stream digest against the receipt.

### F2 MAJOR: run-B guard is a substring test on argv; not tied to the file; no once-only lock
- est_eval.c:311, 320; est_fit.c:15; est_replay.c:453.
- `strstr(raw, "3e9e53be3358")` only. `cd <run B dir>; est_eval --raw machine-state.ndjson ...` (relative path) passes without --recorded and would perform the irreversible evaluation as an unrecorded run. Same hole in est_fit (est_fit would fit on B: it only requires a SHA256SUMS line, not EST_RUN_A_SHA) and est_replay. Symlink/copy also bypasses.
- `--recorded` is accepted for any input (run A gives "recorded": true, the test does this at test_est_tools.c:234-247), so the flag proves nothing about B.
- Nothing stops a second recorded run: `fopen(out,"wb")` overwrites the receipt (est_eval.c:364).
- Fix: identify by content: compare loaded file SHA to the run-B and run-A SHA constants (both are in SHA256SUMS, no data needed), refuse est_fit unless sha == EST_RUN_A_SHA, refuse est_eval without --recorded whenever sha == B sha, refuse --recorded when sha != B sha, open out with "wx" and also refuse if a receipt for B exists.

### F3 MAJOR: receipt provenance is self-declared and forgeable
- est_eval.c:300, 366-370, est_replay.c:15 (constants).
- tool_commit is a free CLI string (not checked against git HEAD, dirty tree, or the binary). protocol_commit is a hardcoded string; the protocol doc digest is not recorded. No digest of est_eval / est_kf.c / est_replay.c. The marks file digest is not recorded (used for the regime split). Strings are printed unescaped into JSON (a quote in --tool-commit breaks or injects into the receipt).
- Parameter file is plain text: `fit_run_path`, `fit_file_sha256` (the A constant), q, r, loglik can be hand-edited and est_params_read accepts it (est_replay.c:423-443). Only the digest of the file lands in the receipt. q and r are not checked to be finite, positive, on the grid; a bad value would make the replay fail and burn the single recorded run.
- Fix: embed `-DEST_TOOL_COMMIT="$(git rev-parse HEAD)"` and refuse a dirty tree at build; put SHA-256 of the protocol doc, marks file, est_eval binary in the receipt; validate q, r are finite and exactly on the 19x25 grid before opening B; JSON-escape strings.

### F4 MAJOR (test gap, receipt claim overstated): prefix-invariance cannot detect any look-ahead
- est_eval.c:340-347, test_est_tools.c:85-106, est_replay.c:300.
- The prefix "truncation" only lowers the loop count; the whole file stays loaded in `f->data`. Code that reads line li+1 gives the same answer in the prefix run and the full run, so the check passes. Proven: mutant that computes the horizon from the NEXT line's t (mut/peek): test_est_tools prefix test PASS (only the gap-specific tests at lines 124-126 failed), and est_eval on run A still writes `prefix_invariance pass: true` and REAL_SIGNAL=PASS.
- The current code is clean: my own harness truncates the FILE itself (head -n k, fresh SHA256SUMS) and diffs the est_replay stream against the prefix of the full stream: 56/56 identical (M0 and M1, k = 31, 32, 45, 71, 73, 100, 199, 204, 206, 500, 777, 1000, 1500, n-1; run A and a damaged run-A copy with bad value, missing key, bad t, duplicate/backward t, deleted lines, 3 s gap). Same harness catches the peek mutant (3/3 mismatches). So no leak today; the gate is just not a leak detector.
- Fix: in est_eval build the prefix est_file with `len` cut at line k and the tail zeroed (own copy of the buffer), then compare.

### F5 MINOR: bad `t` but good value is a valid observation, not a missing one
- est_replay.c:321-327, 344-355 (edge test edge/).
- Protocol s2: "A line that fails to parse ... is a missing observation". Lines with `"t":1790000002.5` (short fraction), leading space, 12 fractional digits, are used as observations with horizon 1 (nominal step assumed) and only counted in bad_t (receipt "bad_t_lines"). Arguably the protocol's "exactly nine fractional digits" makes them parse failures. Zero such lines on A. Decide the rule before B; safest: treat t-parse failure as missing (coast) or state the reading in the receipt.

### F6 MINOR: time parse UB on huge seconds, silent handling of duplicate/backward t
- est_replay.c:210-213, 222, 346-352.
- 12 integer digits allowed, sec*1e9 overflows int64 (signed overflow, UB; edge run "t":999999999999 accepted, horizon clamped to 1e6, later gaps meaningless). Limit to 10 digits.
- gap <= 0 (duplicate t, or t going backwards) silently becomes horizon 1 and prev_wall jumps back; not counted or reported anywhere. Add counters (nonpositive_gaps, horizon>1 count) to the receipt. On A: none.

### F7 MINOR: 10-step coverage includes coasted origins
- est_eval.c:217-219. Loop skips only L<burn-in; a coasted step's posterior (pure prediction) is used as the origin. Protocol s2 "Coasted steps contribute nothing to ... any run B statistic". Targets are correctly non-coasted (line 223). Add `if (s->coast) continue;`. No effect on A (no coasts).

### F8 MINOR: marks handling silently drops data
- est_eval.c:112-123, 95. An unclosed `begin` (run cut off) is discarded, so its samples are labelled idle; >512 pairs silently ignored; lines with other formats ignored. A has 133 pairs, balanced. Regime results on B depend on it. Fix: close an open trial at the last sample time, error on >512, report begin/end counts in the receipt. Also note est_eval will refuse to run if B's SHA256SUMS lacks a marks line; check that line exists first (grep the SUMS file only).

### F9 MINOR: burn-in and quarter definitions are one reading of an ambiguous text
- est_eval.c:157-163, 170, 182; est_fit.c:38.
- "first 30 steps": tools drop L<30 where L=0 is the prior line, so 29 innovations are dropped (n = lines-1-29 = 1923 on A). Reading as 30 innovations gives 1922. Consistent between fit and eval. Quarters are of the post-burn-in, non-coasted logical-time span, not of the whole run "by time". Boundaries move by a few samples. Both are defensible; put the chosen reading in the receipt (n, quarter bounds) so it is on record.

### F10 MINOR: ESTIMATION_CALIBRATION is "any model both calibrated and RMSE<=persistence"
- est_eval.c:362. Protocol s5: "at least one model is calibrated and its RMSE no worse than persistence". Matches the literal text. Note the selected model (line 358) ignores RMSE, so selected_model can be M0 while CALIBRATION=PASS came from M1. Report both; not a violation.

### F11 MINOR: failure record incomplete
- est_eval.c:280-287. failed_criteria omits "rmse vs persistence" and "samples < 3"; no failure-class field (s5 requires the most likely class to be recorded with the FAIL). Add rmse to failed list; the class is a human step but the receipt should have a placeholder or a companion note.

### F12 MINOR: REAL_SIGNAL "both files verify"
- est_eval.c:360. Only the input file is verified by the tool (correct per "evaluation takes params and run B only"). A's SHA appears only as a string in the editable params file. `input_sha_matches_SHA256SUMS: true` is a hard-coded literal (true because load refuses otherwise). Acceptable, but say so in the receipt notes.

### F13 MINOR: Ljung-Box, lag-1 are on the coast-compressed series
- est_eval.c:172, 201-208. Neighbours in w[] can straddle coasts or horizon>1 gaps. Consistent with "exclude coasted"; no effect on A. Mention only.

### F14 NIT
- est_eval.c:320 redundant/unreachable-looking line; est_replay main ignores atoi failure (model id) and needs --recorded first; est_params_read ignores protocol_commit and fit_lines; est_replay.c:340 chain_ok forced 1 for prior and coasted steps (no obs/innov digests exist; reported as such); est_fit prints loglik only for chosen point.

## Findings that need no fix: statistical facts about run A (not defects)
- Fit lands on the grid edge for M0 (q = 10^6, the maximum) and r = 10^5.25; M1 q = 10^5.5, r = 10^5.75. Fitted loglik M0 -16339.9577 over 1923 steps, M1 -16671.4748.
- On A (non-recorded test use), M0: c50 0.7296, c80 0.8357, c95 0.9147 (fail 50 and 95), NIS 1.0542, bias 0.00046, lag1 0.0820, RMSE 1184.966 vs persistence 1182.546 (fails persistence), 10-step 0.9843. M1 fails c50, c95, quarter 4, RMSE 1408.4. CALIBRATION=FAIL on A. That says the model class is wrong for this signal (heavy 1 C sample noise, data rms 1 s-diff 1175 mC), so a FAIL on B is the likely outcome.
- Test-driven alarm (protocol s5a): zero-innovation fraction on A is 2.0 %, so the quantization story is unlikely to be the class.

## What held up (checked line by line against the protocol)
- Wilson: formula and z = 1.959964; reproduces 0.90139142653388/0.92638814642904 for 1759/1923.
- z for 50/80/95 = 0.674490/1.281552/1.959964, inclusive `<=`. NIS from the KF's Cholesky y'y = nu^2/S for m=1. Bias mean/sd with n-1. Lag-1 on demeaned nu/sqrt(S), lag k=1..10 Ljung-Box Q = n(n+2) sum r_k^2/(n-k). Log score = -0.5(log 2 pi S + nu^2/S). RMSE and persistence (last observed value, coasted steps do not update it; prev_z updated during burn-in).
- Ten-step coverage uses the horizon-10 predicted variance (est_kf_predict with 10), target found by exact logical time, target non-coasted. The KF applies horizon by iterating unit steps; for M1 that equals Q(dt=h) exactly (semigroup: h=2 gives q[[8/3,2],[2,2]]), so the protocol's Q(dt) is honoured.
- Grid 19 x 25 exactly (10^-3..10^6 step 0.5; 10^0..10^6 step 0.25). Fit excludes prior, coast, L<30; tie-break by strict `>` with q outer, r inner = smaller q then smaller r. Prior x0=[first valid,0], P0=diag(r,1e6). Horizon = max(1, round-half-up(gap/1s)). Logical grid time on prediction and observation; wall t kept in raw line and evidence digest (SHA of raw line without newline); source digest text correct.
- Bands, NIS [0.80,1.25], bias <= 0.10, lag1 <= 0.20, quarters and regimes [0.90,0.99], regime gate at >=100, ten-step band, calibrated = all of them, selection = simplest calibrated within 0.01 of best calibrated (M0 first): matches s5 exactly. REAL_SIGNAL and CALIBRATION verdict expressions match s5 as written.
- Fit never sees B by design: est_fit takes one path (subject to F2). Eval has no fit code and does not call the fit loop; it reads q, r from the file.
- Receipt carries protocol commit, tool commit (declared), parameter digest, input digest, recorded flag, coast and bad-line counts (subject to F3).
- Coasting: missing/NaN/absent key/garbage lines coast, are counted, and are excluded from fit, all one-step stats, quarters, regimes, persistence, log score. Verified on the damaged copy (2 coasts, 2 bad values, 1 bad t counted).
- Marks parse and regime split by wall t inclusive on both ends; regime with <100 samples ungated.

## Independent recompute on run A (own scratch C written from the protocol, own scalar/2x2 KF from raw lines and marks; plus awk on est_replay's stream)
Compared with est_eval normal non-recorded invocation, params from est_fit on A:
- M0 n=1923; c50 0.7295891836, c80 0.8356734269, c95 0.9147165887, mean NIS 1.0542315396, bias 0.0004637656, lag1 0.0820417582, LB10 151.761920, log score -8.4971179035, RMSE 1184.966345, persistence RMSE 1182.545533, quarters 0.908524/0.906445/0.939583/0.904366, regime idle n=1 / trial n=1922 cov 0.915193, ten-step n=1913 cov 0.9843178254.
- M1 n=1923; c50 0.6983879355, c80 0.8315132605, c95 0.9167966719, NIS 1.0369062669, bias -0.0001759079, lag1 0.1219188398, LB10 325.788689, log score -8.6695136882, RMSE 1408.446763, quarters 0.931393/0.918919/0.927083/0.889813, ten-step n=1913 cov 0.9874542603.
- All agree with est_eval to the printed digits (10+ significant). My brute-force grid search over the 19x25 grid (own code) picks the same argmax and loglik for both models (M0 i=18,j=21; M1 i=17,j=23).
- Ljung-Box, Wilson and prefix streams agree too.

## Test gaps (mutants built in scratch, full test_est_tools run for each)
Nine mutants, full suite: 7 of 9 still PASS with the bug in.
1. Prefix test (test_est_tools.c:85): look-ahead mutant passes it (F4). Only unrelated gap tests caught the mutant.
2. Coast exclusion in statistics (remove `if (s->coast) continue;` in compute, est_eval.c:169): suite PASS. The eval tests use undamaged synthetic data (make_series damage=0 for the eval series, test_est_tools.c:169-170), so coast handling in fit/eval statistics is untested; only the replay flags are.
3. Ten-step horizon (predict with horizon 1 instead of 10, est_eval.c:226): suite PASS, because the synthetic series has r=10^3.5 >> 10 q=10^2.5 so the 10-step interval barely changes. Use q >= r/10 in a synthetic test with a known ten-step coverage.
4. Also passing with the bug: bias divisor n instead of n-1; Wilson missing the z^2/2n term; EST_BURN_IN 20 instead of 30; fit tie-break `>=`; regime labels inverted (idle/in-trial swap). No test checks a Wilson value, bias value, burn-in count, tie-break, or regime split with distinct coverage per regime. The quarter mutant (last quarter merged) was caught only because it broke the right-model calibrated check.
Suggested tests: fixed-value checks on a hand-computable tiny series (n, hits, Wilson, bias, lag1); synthetic damaged series through est_eval with an expected n; two-regime series with different noise; a tie grid test; a prefix test on a truncated buffer.
