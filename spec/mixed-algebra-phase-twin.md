# Omega mixed algebra: phase-domain Z3 digital twin (MA-8 step 0)

Status: ADR 0019 **MA-8 step 0**, a simulated precursor of MA-8 ("physical
phase or external-Machine realization"). It is not MA-8 itself: there is no
physical device. Provenance of every result is `SIMULATED_DEVELOPMENT`
(ADR 0018 AR1/AR2 wording). It makes no speed, latency, energy or physical
claim, does no selection or ranking, and mints no semantic id or digest. It
may be cited as model-only evidence toward the Turing gate
DIGITAL_PHASE_EQUIVALENCE; it does not claim that gate.

Code: `src/algebra/phase_twin.{h,c}` (twin), `tests/algebra/test_phase_twin.c`
(gates and receipt). Receipt: `evidence/MIXED_ALGEBRA/phase_twin_receipt.<sha256>.json`.
Commands: `make test-phase-twin` (full run into `build/`, quick run plain and
under ASan+UBSan with byte-identical receipts, reproduction check against the
committed receipt; about 90 s wall on the Spark), `make phase-twin-receipt`
(clean committed tree only; writes the content-addressed receipt). C11 + libm,
no other dependencies, CPU only.

Sources: ADR 0019 (sections 3, 5, 6.3, 8), ADR 0018 (sections 3, 12),
`docs/plans/mixed-algebra/MIXED_ALGEBRA_THEORY.md` section 6,
`docs/plans/mixed-algebra/research/PHASE_EXPERIMENT_AND_HARDWARE.md`
("research note"), all at aien-architecture `origin/main` 1438799. Oracle:
`oma_z3_add` / `oma_z3_mul` (`src/algebra/oma_z3.c`).

## Claim form

Every result below has the form: under model profile P (the declared
parameters), candidate realization C versus the oracle `oma_z3`, measured X,
from the receipt. Nothing transfers to hardware without AR4-grade physical
evidence.

## Model

Encoding: Z3 value k is the tone `A cos(2 pi n / 64 + 2 pi k / 3)`, so
`f_c = f_s / 64` and the phase is 0, 120 or 240 degrees. Converter full scale
is +/-1; A = 0.5 (-6.02 dBFS). Converters are mid-tread quantizers with
clipping (`bits = 0` means ideal).

Addition is phasor multiplication (theory 6.1, 6.2). Three candidate
realizations:

| rz_id | mechanism | where the multiply happens |
|---|---|---|
| PT_corr_product | two channels emit a and b; each is I/Q-correlated at f_c; the two correlator outputs are multiplied | complex multiply of measured phasors (research note loopback) |
| PT_mixer | two tones enter an ideal analog mixer; the product is digitized and correlated at 2 f_c | analog multiply (simulated) |
| PT_nco | 32-bit phase words `a T + b T` (T = floor(2^32/3)) wrap-add; one tone at that phase | integer phase accumulator (NCO) |

Measurement: `I = sum x[n] cos(2 pi n / 64)`, `Q = -sum x[n] sin(2 pi n / 64)`
(bin 2 for mixer and doubler). Decision: `k = mod(round(theta / 120 deg), 3)`,
cells +/-60 degrees.

Calibration (research note "Timing and calibration"): every
`interval` operations, emit k = 0 on each path `reps` times, average the
correlator outputs, store the phase as the path offset and the magnitude as
the erasure reference; then emit k = 1 `reps` times and require
120 +/- `tol` degrees after correction, else a calibration fault. Later
correlator outputs are rotated by the stored offset. The reference is
time-multiplexed on the signal paths (the research note's procedure); a
dedicated always-on pilot tone is the hardware alternative and is not modelled.

Flags (each fails the trial; no voting): erasure (magnitude below 0.3 x
reference), reject (output phase more than 30 degrees from the nearest
center), calibration fault (every operation in a block after a failed check).

Noise and impairments: AWGN per sample at the ADC input (std `sigma`,
independent per channel); one Gaussian phase jitter per channel per frame;
DAC and ADC quantization; fixed per-channel path offset; linear per-channel
drift in simulated time (`frame_s` per frame, calibration frames included).
Symbol SNR at the correlator output is `rho = A^2 N / (4 sigma^2)`, equal to
the research note's `rho = N SNR_1 / 2`.

Declared simplifications: the analog path is a pure phase shift at f_c applied
before the DAC; noise is white Gaussian; converters have no INL/DNL or spurs;
mixer and doubler are ideal multipliers; one shared clock (no sample slip, no
frequency offset); no crosstalk; no amplitude drift.

RNG: xoshiro256** seeded by splitmix64, Gaussian by the Marsaglia polar
method, fixed seed per scenario (listed in the receipt). No clock or host data
enters the receipt except the recorded commit, dirty count and compiler
version, so plain and sanitizer builds write byte-identical receipts.

## Profiles

| profile | f_s | N | bits DAC/ADC | use |
|---|---|---|---|---|
| A | 125 MS/s | 4096 | 14/14 | research note reference design |
| B | 1 MS/s | 1024 | 12/12 | research note low-rate design |
| B-ideal | 1 MS/s | 1024 | ideal | quantization-free control |
| SWEEP | 1 MS/s | 64 | ideal | gate 2; flags, jitter, offset, calibration off |
| P (contract) | 1 MS/s | 64 | 12/12 | gate 3; frame 128 us, rho 25 dB per input, jitter 1 deg rms, flags on, calibration every 1000 ops with 32 reference frames, tol 5 deg |

## Pre-registered gates and thresholds

These thresholds are fixed in this document and in `tests/algebra/test_phase_twin.c`
before the committed receipt is generated.

- **Self-checks.** Decoder boundaries (59.9 degrees -> 0, 60.1 -> 1), NCO wrap
  pair (2,2) -> 1, argument rejection, RNG moments, the analytic add reference
  stable between grids of 3600 and 7200 points (relative 1e-3), Craig integral
  at 0 dB within 5e-4 of 0.1834.
- **Gate 1, noiseless (exhaustive).** Profiles A, B, B-ideal x realizations
  PT_corr_product, PT_mixer, PT_nco: all 9 input pairs x 1000 repetitions
  decode equal to `oma_z3_add`, zero mismatches, zero flags. Multiply by a known
  constant on profile B (calibration on): 9 pairs x 1000 equal to `oma_z3_mul`.
  Quantization sweep 16 to 1 bits is informational only.
- **Gate 1b, research note pass condition.** Profile A, `SNR_1` = 80 dB per
  sample, 1 ps jitter (7.0e-4 degrees at 1.953 MHz), 3.35 degree fixed offset
  on channel b (1 m cable), calibration on: 9 x 1000, zero failures.
- **Gate 2, error rate versus SNR (PT_corr_product, profile SWEEP).** At rho
  = 0, 6, 8, 10, 12 dB per input with 1e5, 2e5, 4e5, 1e6, 4e6 trials:
  (a) the exact add reference (numerical circular convolution of the exact
  PSK phase density, 7200 points) lies inside the measured Wilson 99.9 %
  interval; (b) the research note's Monte Carlo add values (3.52e-1, 5.76e-2,
  1.52e-2, 1.96e-3, 8.46e-5) are within 5 % of the exact reference at 10 and
  12 dB; (c) the single-phasor read of channel a matches Craig's exact 3-PSK
  error inside Wilson 99.9 % wherever at least 100 errors are expected, and
  Craig matches the research note's table within 1 %; (d) pure 15 degree
  per-input jitter, 4e5 trials: `2 Q(60 / (sqrt2 x 15))` = 4.68e-3 inside
  Wilson 99.9 %.
- **Gate 3, calibration (profile P).** Cases offset-only (50, 20 degrees),
  drift-only (2.0, 1.5 degrees/s), offset and drift. Uncalibrated runs
  (2e5, 1e6, 2e5 trials) must **violate** the contract: Wilson 95 % lower
  bound above 1e-6. Calibrated runs (2e5, 1e6, 4e6 trials) must have zero
  failures. The contract case (offset and drift, 4e6 trials) must meet the
  contract below.
- **Gate 4, receipt.** Records every profile parameter, the contract, measured
  error rates with Wilson bounds, evidence tiers per ADR 0019 section 8, the
  Z3 multiply verdict, and field names from `src/turing/field.h` where they
  apply (`rz_id`, `algebra`, `representation`, `tier_source`, `run_id`,
  `git_commit`, `tree_dirty`, `toolchain`, `verified`).

Contract (ADR 0018 `BOUNDED_STOCHASTIC`): operation Z3 add `(a + b) mod 3`,
realization PT_corr_product at profile P; bound: probability of a failed
operation (mismatch or any flag) <= 1e-6; confidence 95 %; required: zero
failures in at least `ceil(-ln 0.05 / 1e-6)` = 2,995,733 trials, and the
two-sided Wilson 95 % upper bound <= 1e-6 (hence 4,000,000 trials).

### Pre-registration record

The thresholds above were written into the test code before the first full
run, but that run (uncommitted working tree) came before this document. One
parameter changed after it: profile P first used 8 calibration reference
frames. At rho 25 dB the k = 1 check then misfires about once per 4,000
calibrations (6e-5 per check); one such false calibration fault flagged a
block of 1,000 operations in the drift-only and contract cases. The frame
count was raised to 32, which puts the check tolerance at about 8 standard
deviations. The contract bound, confidence, SNR, trial counts, tolerances and
flag thresholds did not change. This is a post-observation change to a
calibration parameter and is recorded here as such.

## Z3 multiplication

Two unknown operands: **not realizable in this model.** Phasor multiplication
realizes Z3 addition. Z3 `a b` corresponds to `w^(a b) = (w^a)^b`, which is
exponentiation, and no bilinear phasor operation computes it (theory 6.4). A
decode-then-select circuit (decode b digitally, then identity, doubler or
constant source) is a hybrid digital realization and is not claimed.

Multiplication by a known constant b: **realized** (gate 1): b = 0 is a constant
k = 0 source, b = 1 identity, b = 2 a frequency doubler (squarer read at
2 f_c), since `w^(2a) = conj(w^a)`.

## Evidence tiers (ADR 0019 section 8)

| item | tier | scope |
|---|---|---|
| add, noiseless, each profile x realization | E3 (all 9 pairs) | the twin model; the ideal map itself is E4 (theory 6.1 proof) |
| mul by known constant, noiseless | E3 | the twin model |
| add, bounded contract at profile P | E1 | the twin model only |
| SNR curve and jitter check | model validation, not a contract | agreement with analytic references |
| mul of two unknowns | E0: no realization exists | |

## What the twin proves and what it does not

It shows that, inside the declared model, the phase encoding plus the three
add mechanisms compute exactly `oma_z3_add` on the whole Z3 domain; that
the decoder's error statistics match the analytic 3-PSK results, so the
contract arithmetic is right; and that the reference-frame calibration turns
a failing channel (offset and drift) into one that meets a 1e-6 bound at
rho 25 dB.

It does **not** show that any physical board does this. ADR 0018 forbids a
physical analog claim without physical evidence, and there is none. It makes
no claim about speed, latency, energy, or cost against the digital
`oma_z3_add`. The noise models are idealized (list above); real converters,
cables, mixers and clocks have effects the twin leaves out.

## What a real board must meet for the contract to transfer

A board inherits the twin's contract only if measurements (AR4 evidence)
show all of these at the operating point used:

1. One clock and PLL for DAC, ADC and NCOs; NCOs cleared on the same cycle;
   no sample slip (one sample at `f_s / 64` is 5.625 degrees).
2. Symbol SNR at the correlator output >= 25 dB per input:
   `N SNR_1 / 2 >= 316`, measured with the board's effective bits, at -6 dBFS
   and no clipping.
3. DAC and ADC >= 12 effective bits over the used band, no spur at f_c or
   2 f_c above the noise that the SNR figure assumes.
4. Frame-to-frame phase jitter <= 1 degree rms per channel (1.42 ns rms at
   1.953 MHz; 178 ns at 15.625 kHz).
5. Any fixed path offset, with the k = 1 check passing (120 +/- 5 degrees).
6. Phase drift <= 2 degrees/s per channel with recalibration every 1,000
   operations (0.13 s at profile P), or proportionally faster recalibration;
   board temperature within 2 degrees C across a block.
7. 32 reference frames per calibration step at rho 25 dB (fewer at higher
   SNR, as long as the check's false-fault rate stays below the contract).
8. Noise white and Gaussian enough that the measured error curve matches the
   gate 2 curve; independent channels; crosstalk below the noise floor.
9. For PT_mixer: mixer linear enough that the 2 f_c product term dominates,
   and the ADC sees the product without clipping.
