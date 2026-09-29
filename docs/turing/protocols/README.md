# Turing Validation Protocols v1.0

Two companion documents that specify how the Turing unit gets calibrated from a
defined statistic into an empirically validated measurement. Published so an
independent laboratory can replicate the program.

## Documents

1. **Turing Instrument Calibration and Validation Program v1.0**
   ([PDF](turing-instrument-calibration-validation-protocol-v1-0.pdf),
   [LaTeX source](turing-instrument-calibration-validation-protocol-v1-0.tex),
   27 pages)
   The scientific protocol: the ordered qualification gates (CAL-0, EXP-001,
   EXP-002A/B/C/D, EXP-003, H3, H4, H5), the frozen measurement profile fields,
   preregistration and blinding rules, the claim ladder, and the final
   qualification matrix. Each gate is designed so it can fail.

2. **Turing Laboratory Execution Protocol v1.0**
   ([PDF](turing-laboratory-execution-protocol-v1-0.pdf),
   [LaTeX source](turing-laboratory-execution-protocol-v1-0.tex),
   42 pages)
   The engineering specification that makes the calibration program executable:
   repository layout, artifact schemas and digest rules, the `turing` CLI,
   statistical procedures, per-experiment runbooks, agent assignments,
   hard acceptance gates, and the definition of done (an outside reviewer can
   reconstruct every result from the artifacts alone).

## Reading order

Read the calibration program first (what must be proven and in what order),
then the execution protocol (how to build the laboratory that proves it).

## Status

v1.0, September 29, 2026. By M. Drake Stapleton, AIEN Project.
Companion to the preprint "Computing Machinery and Understanding:
From the Imitation Game to the Turing".
