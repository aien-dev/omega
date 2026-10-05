# ESTIMATION v5: result HELD_OUT_FAIL (selected G1 not calibrated on D2)

**Verdict: `ESTIMATION_CALIBRATION (v5) = HELD_OUT_FAIL`**, reason "S not calibrated",
first failing statistic `pit_bin0`. Receipt: `receipt-7f0bb7018d066bc438e7a8a9bf87c0d45615f60be2d029b6ab5194168e3db3d1.json`
(tool commit `eec20d2`, tool_dirty 0, synthetic_test false). Scored once on
2026-10-05; the tool refuses a second receipt. EST-3 stays FAILED (v1 to v3), v4 stays
INCONCLUSIVE, and EST-4 and EST-5 stay blocked.

Protocol: `docs/estimation/protocols/est-v5.md`, sha256
`275cd5b72570d5182ef73d635e16775026e9d911e818b8e12f8300a701187af5`, frozen at
`b37e34b`, unchanged. Params sha256 `6be2b579980f22b48d8224b36dec39b6154cc82c15a85979291672b2c5c2aa3a`,
unchanged since D1. No threshold, rule, tool or parameter was changed after D2 was collected.

## D1 collection

| Attempt | Seed | Window | Outcome |
|---|---|---|---|
| 1 | 0xE5C5D1 | 111 foreign samples (cargo, rustc, make, cc, test_osc_backend) 15:01:57 to 15:07:32Z although the quiet flag was held | void, aborted at 15:31Z after 1824 s, never fitted or scored (`evidence/EST5/raw/20261001T150104Z-est5-fit-silicon/`, INVALID) |
| 2 | 0xE5C5D2 | `WINDOW_CLEAN samples=1317 foreign=0`, 15:31:56 to 16:17:07Z | used as D1 (`evidence/EST5/raw/20261001T153156Z-est5-fit-silicon/`) |

Attempt 1 was voided on the process monitor alone (protocol section 7), before any
thermal value was read. The cause is that the quiet-guard hook only reaches sessions
and commands that load it; the coordinator then stopped every lane and attempt 2 saw
no foreign build, test or gate process (`process-monitor.log` in the folder, 0
FOREIGN lines; `WINDOW` file; both in `SHA256SUMS`). The v3 pre-check on D1 is
VALID (2682 lines, 0 of 2681 gaps above 1.5 s, marks ok, foreign mean 0.2787 busy
cores, 0 samples above 3.0, 0 unmeasured). The only informational HIGHCPU lines
were other sessions' interpreters, not builds or tests.

## D2 collection (sealed held-out)

| Attempt | Seed | Window | Outcome |
|---|---|---|---|
| 1 | 0xD2E5C6 | `WINDOW_CLEAN samples=1312 foreign=0`, 11:28:38 to about 12:14Z on 2026-10-05 | used as D2 (`evidence/EST5/raw/20261005T112838Z-est5-heldout-silicon/`) |

Before collection every other session on the machine confirmed it had stopped builds,
tests, QEMU and GPU work, and both idle `clangd` language servers were stopped (the
collector refused its first start because of them; that refusal collected nothing).
Quiet flag held by L10-EST5D2 for the window. Raw files committed with `d2.sha256` in
`eec20d2` before scoring. Pre-check: VALID (2681 lines, 0 of 2680 gaps above 1.5 s,
marks ok, foreign mean 0.4138 busy cores, 0 samples above 3.0, 0 unmeasured).

## D2 score (the one binding run, `est5 recorded`)

| Family | D2 one-step mean log score | Calibrated on D2 |
|---|---|---|
| G1 lag (selected) | -1.80307 | no (first fail `pit_bin0`) |
| E0 baseline | -2.10541 | no |
| F1 baseline | -2.97852 | no |

G1 beat both baselines (same steps, tie allowance 0.01 nats) and passed 22 of its 23
gated statistics on 2651 scored steps: coverage50 0.533 (band 0.46 to 0.54), coverage80
0.822 (0.76 to 0.84), coverage95 0.953 (0.93 to 0.97), PIT bins 1 to 9 inside 0.07 to
0.13, and the quarter, regime, ten-step, bias and lag-1 checks. It failed one:
**PIT bin 0 = 0.0681, below the lower bound 0.07**. That bin holds steps where the real
temperature fell in the lowest tenth of the forecast range; too few of them means the
forecast puts slightly too much weight on low temperatures (a low tail a little too wide, or forecasts a little low; the run does not say which). Under section 6 a single failed gated
statistic means not calibrated, so the verdict is HELD_OUT_FAIL. The margin is small
and is recorded here only as description; it does not change the verdict.

## Phase A (the one binding fit, `est5 fit`, params sha256 `6be2b579980f22b48d8224b36dec39b6154cc82c15a85979291672b2c5c2aa3a`)

`phase_a PASS`, selected **G1** (all three families pass the in-sample screen on 2652
scored steps).

| Family | D1 one-step log score | D1 ten-step log score | Screen |
|---|---|---|---|
| G1 lag | -1.96281 | -3.72398 | pass |
| G2 AR change | -1.96411 | -3.71924 | pass |
| G3 two-tank | -1.97846 | -3.62463 | pass |
| F1 baseline | -2.94238 | | |

G1 parameters: tau 1 s, q 1e4, lambda 0.1 (on the grid edge, flagged only), nu 1.25,
c 0.7, phi 0.95, nu_h 0.8, c_h 0.2. These numbers are in-sample and carry no
held-out meaning.

## Limits (read before quoting any v5 outcome)

The following is the external statistical review (Astra, via OpenCode; Grok was
unavailable, out of credit; Gemini agreed the estimator is sound and proposed model
changes that v5 does not adopt), quoted:

> The proposed PASS is meaningful as a **predeclared operational acceptance check**,
> but it is **not a statistically established certificate of calibration**. Its
> overall false-pass and false-fail rates are unknown.

> The important result is that a nominal 95% interval that actually covers **92%**
> can pass about **88% of the time**. Its miss rate is 8% instead of 5%, which is
> **60% more misses than advertised**.

> A clean PASS under the present rules would license this claim: On this new run
> from the specified workload generator, the frozen forecasting procedure met our
> declared accuracy, coverage, and comparison thresholds. It would **not** yet
> license "statistically validated 95% calibration," a known overall false-pass
> rate, physically identified thermal parameters, or safety guarantees.

Further limits recorded from that review: mid-PIT z checks (bias, lag-1) are
descriptive on a discrete predictive; the F1 comparison has no sampling allowance;
the result covers new schedules of the same load generator only (not other
workloads, ambient conditions, boards or horizons beyond 1 and 10 s).

## Downstream

Nothing downstream may treat any estimator output as calibrated. EST-4 and EST-5
stay blocked. v5 is closed: a further attempt needs a new protocol version (v6) with
its own freeze, fit and sealed held-out run; nothing in v5 may be re-scored or retuned.
