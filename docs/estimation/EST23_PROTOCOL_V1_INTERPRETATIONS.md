# EST-2/EST-3 protocol v1: tool readings

Companion to `EST23_PROTOCOL_V1.md` (frozen at commit f96dc97, not edited). It
records how the tools read the places where the protocol text can be read more
than one way. No threshold, band, grid value or model is weakened or changed.

1. Step 0 is the first valid line. It is the prior, and burn-in excludes every
   step with L < 30, where L counts from that prior line (L = 0 is the prior).
   So 29 innovations are dropped after the prior.
2. Horizon rounds the wall-clock gap half up: (gap + 0.5 s) / 1 s in integer
   arithmetic, minimum 1. A gap of zero or less (duplicate or backward `t`) gives
   horizon 1. Those cases are counted in the receipt.
3. Every line after the prior gets one step.
4. A missing observation coasts with a zero observation digest and is excluded
   from every statistic, including ten-step origins. A line whose `t` fails to
   parse is a missing observation even when its value parses. A seconds field
   longer than 10 digits, or one that would overflow 64-bit nanoseconds, is a
   `t` parse failure and is counted separately.
5. `thermal_mc` must be a plain decimal. Hex, nan and inf are missing.
6. Ten-step coverage uses logical time and the horizon-10 predicted variance.
   Origins that were coasted are skipped; targets must be non-coasted steps.
7. Quarters split the included samples by logical time into four equal spans.
8. Lag-1 autocorrelation and Ljung-Box run over included, non-coasted
   innovations, using the sample mean.
9. Standardized bias uses the n-1 standard deviation.
10. The persistence baseline is the previous valid observation, over the same
    included steps.
11. CALIBRATION PASS means some model is calibrated with RMSE no worse than
    persistence.
12. Extra reported statistic: the fraction of exactly-zero innovations.
13. An unclosed `begin` in the marks file runs to the end of the file and is
    reported in the receipt, together with begin, end, nested-begin and stray-end
    counts.

These readings were fixed before any run on run B; they change no band, grid value or model.
