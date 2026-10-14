# Ionic-droplet reservoir: pinned reference and Gate 2 contract (omega#440)

Status: Gate 0 (reference pinned), Gate 1 (CPU simulator, tests) and Gate 2
(comparison against the archived reference outputs, benchmarks, timing) run
on branch `research/reservoir-440`; Gate 2 results and their receipt are in
section 11. Gate 2 result in one line: the three Lorenz networks PASS, the
one comparable NARMA case FAILs (an ill-conditioned reference fit, section
11.1), 63 NARMA cases are BLOCKED, and no reservoir beats the conventional
baselines in the benchmarks. The authors' MATLAB was not run: no MATLAB or
Octave on this machine (section 5).

The design of `src/reservoir/` is Omega's own. The authors' code and numbers
are used only as a correctness reference: no line of MATLAB was ported, the
model was re-derived from the equations below, and every place where Omega
differs from the reference method is listed with the measurement that
justifies it (section 6).

Tags used below: **[code]** read from the archived MATLAB (file:line),
**[paper]** stated in the article or supplement, **[archive]** read from an
archived `.mat` file, **[measured]** produced by `make test-reservoir` on
this branch, **UNVERIFIED (p%)** a claim not checked by a tool, with a
confidence.

## 1. Provenance

| Item | Value |
|---|---|
| Article | Armendarez, Mohamed, Hasan, Najem et al., "Neuromorphic tissues: Soft biomolecular networks for brain-inspired temporal computing", Science Advances 12(30), eaed0971 (2026). DOI 10.1126/sciadv.aed0971, PMC13394382. License of the article: CC BY-NC 4.0. |
| Supplement | Texts S4 (network model), S5 (parallel reservoir), S6 (error measures), local copy `/tmp/res440/paper/sm.txt` |
| Code and data | Zenodo record 17296788, DOI 10.5281/zenodo.17296788 (concept DOI 10.5281/zenodo.17296787), published 2025-10-08, no version string in the record metadata. Record license: CC-BY-4.0. Creators: Armendarez N., Mohamed A., Hasan M. S., Najem J. |
| Archive file | `repo.zip`, md5 `ad2c5057dfeafe890e624d8b1174f5d8`, sha256 `54582293dff8e9327b9187d3e5526b0e0cdcc41ca7ac87c762850bdb342dea15` |
| License inside the archive | `LICENSE` says MIT (Copyright 2025 Ahmed Salah Mohamed, Joseph Najem, and collaborators); `CITATION.cff` also says MIT with DOI "10.5281/zenodo.TBD". The record says CC-BY-4.0. Omega copies no code; the topology tables in `tests/reservoir/res_fixtures.h` are data taken from the archive with this attribution, which satisfies both licenses. |

Files relied on (sha256 of the archived copy; paths under
`src/Codes for Zenodo/` or `data/raw/Codes for Zenodo/`):

| File | sha256 |
|---|---|
| NARMA Final Code/Connected Reservoir/HyperParameterTuningOptimized.m (below: **H**) | `620640f002068048ab2c56f5090dc945e7b81595a9bc7f863331659a1850ceb5` |
| NARMA Final Code/Parallel Reservoir/ParallelReservoirNARMA.m | `f120f9150db590e3fcf767cbd1840e0924f513b45d44cf8439be2229be0fb507` |
| Lorenz Final Code/Lorenz63SolverCustomRegression.m (43 nodes, below: **L**) | `d291460fffe4efc1b2f013060c04e3a561dcd571de813a9c95277557daafdcac` |
| Lorenz Final Code/Lorenz63SolverCustomRegression13Nodes.m | `c9e2ad2b64832c50c95f9d61e86161496aef40640cc547d1d5ae3a9323eedbf1` |
| Lorenz Final Code/Lorenz63SolverCustomRegression25Nodes.m | `c1b31403cc9a2014c2f3b65056873306ed52174142259b5127b30c803927dece` |
| Lorenz Final Code/LorenzDataGenerator.m | `7295d55fa3e1209b74b4804f9de18f4952950020f32d0f318695286c5ccdfa1d` |
| Connected Reservoir/Length5.mat | `dc6e7914d216a21f664adcd4843e9f705616df47511879ba0bbeceb241b9b50a` |
| Connected Reservoir/Length10.mat | `ceca1f6e94f3239b2df072403a488f8929fe00673b5028319cd2094d6eae708e` |
| Connected Reservoir/Length20.mat | `aeb87fcec7655ce72386fdc28f2878b1f7b89097db523ab9fe5812fb6c4d1ee6` |
| Connected Reservoir/Length50.mat (identical copy under Parallel Reservoir/) | `ff4c9d14abb6f64b1ab9f301072dc67316b7fd59fe61d0b7632ee467a8c2aae3` |
| Connected Reservoir/trainSignal.mat | `48e8e2c775fda4ca85dfc14a3c202319d9c75e2d7db0baf237744ce51a4b02c1` |
| Connected Reservoir/testSignal.mat | `f0e34c5733e2cc20852caf4874b92bcb19fcb95bd4da4ce0eb7f4919cabee8c1` |
| Connected Reservoir/HyperParameterOptimizationVariablesToPlot.mat | `6311b719d47bfa92b76e1c71b3e9fe0e2e30d564d9ee7c7b2f231a03683e66fd` |
| Parallel Reservoir/ParallelReservoirDataToPlot.mat | `ccf13dd5045a50ce5e474915225f4961d6e56b949985ef701bffe3316822f1c6` |
| Lorenz Final Code/LorenzData1.mat | `6229d996681ef7e64336ef4f04e7bd82c0a8063e6e18e52667c699251091f6c4` |
| Lorenz Final Code/LorenzData2.mat | `d1c0dcccbee809e8880dd5b0e355dd456ab1605d0470dbfa99e1a54b2b51073a` |
| Lorenz Final Code/Figure5_Data.mat | `f6055bc6b79cc686a5c7cca703fdf2e35de50e202e245bfe07f10f91ecad3500` |
| Lorenz Final Code/Figure5_Data13Nodes.mat | `74988e19bd24540b0f623e20295a2813a0206917cdf6072d0dfaf69f8d091ed7` |
| Lorenz Final Code/Figure5_Data25Nodes.mat | `0d85b0eaef9c6bb4eed07a3fe8f395802afbde0c3a75403ba216b55220754205` |
| Lorenz Final Code/LorenzNormalizedNRMSEs.mat | `514ba3082a21eb541e0af8a9fbb80596742f9ddafee466391a00842a5d4e5616` |
| Lorenz Final Code/LorenzNormalizedNRMSEs13Nodes.mat | `5f2d18b150d5baa9e792aa967d46a541a6031ff2805467ba30b72db745d55733` |
| Lorenz Final Code/LorenzNormalizedNRMSEs25Nodes.mat | `881e92a0a5cd6b46fb588b41593e83d66ec2af25d16fb7198444fc82c3aaf14d` |

All 19 `.mat` files are MAT version 5 (header "MATLAB 5.0 MAT-file,
Platform: PCWIN64"), data elements zlib-compressed (miCOMPRESSED). None is
v7.3/HDF5. They were read with a throwaway reader outside this repository;
nothing in the archive was executed or built.

### 1.1 A newer version of the code and data exists (not the pinned reference)

**[archive]** Zenodo concept 10.5281/zenodo.17296787 has a second version,
record 20277956, dated 2026-05-18, CC-BY-4.0, one file
`Neuromorphic_Tissues_Manuscript_Repo.zip`, 2,584,641,526 bytes, md5
`61c10efb77f6140882ab16c6ab25dbe5` (record metadata, read by the
orchestrator through the Zenodo API). The pinned reference stays version 1
(record 17296788): it is the version the paper's data-availability statement
cites, and Drake's decision is to compare against its archived outputs.

Version 2 was examined without downloading it, from its zip central
directory (last 1 MiB of the file, HTTP range request) and one 8 KiB range
holding one changed file:

- 78 entries, 2,586,892,630 bytes uncompressed.
- All 19 version-1 `.mat` files are present with the same size and CRC-32.
- 5 of the 6 `.m` files are identical (size and CRC-32). The exception is
  `HyperParameterTuningOptimized.m` (v1 32,353 bytes, CRC-32 aaf83c2e; v2
  32,689 bytes, CRC-32 001a836f, sha256 of the extracted v2 file
  `125187e7697113c25186345eff93f3baba8ae16a0d96e2465720a50adcbe6aed`). Its
  header now says "Edited filepath to work in arbitrary folder", and the
  diff has two changes: the Length file path (v2 lines 54-58), and the
  readout features, which now append squared states, `[statesSampled,
  statesSampled.^2]` with twice the state columns (v2 lines 168, 176-177).
  That is the paper's "augmented with squared states" (discrepancy 6) and is
  a functional change, despite the v1 header.
- New in version 2: `src/.../Connected Reservoir/HyperParameterOutputAll.mat`
  (2,506,601,735 bytes, the bulk of the archive; by its name the
  `save('HyperParameterOutputAll.mat')` of the whole workspace at v1 H:212,
  which would include the ode15s state trajectories `xTrainCell`, UNVERIFIED
  (60%), not downloaded), a second
  `HyperParameterOptimizationVariablesToPlot.mat` under `src/` (14,525,399
  bytes, CRC-32 fdea4aa1, differs from the v1 copy under `data/raw/`),
  `Trainsignal.mat` and `Testsignal.mat` (about 1.4 MB each), editor
  autosave files (`.asv`), a manuscript PDF and manifest files.

The version 2 `src/.../HyperParameterOptimizationVariablesToPlot.mat` was
then fetched on its own (one HTTP range of its 14,239,572 compressed bytes at
the offset the central directory gives, spliced into a sparse copy of the
zip and extracted with `unzip -p`, which checks its CRC-32 fdea4aa1). The
extracted file is 14,525,399 bytes, sha256
`317af7a76051e6c30aa0476a612c29e417b199c9ef226f4f00d7f7505512fc92`. Read with
the in-house MAT reader (section 7.1 item 10), compared with the version 1
copy:

- Same variables. The four option vectors and all 576 non-empty target cells
  (`yTrainCell`, `yTestCell`) are bit-identical.
- All 576 non-empty prediction cells differ, and 504 of 640 `NRMSEtrain` and
  495 of 640 `NRMSEtest` entries differ. In each file the stored NRMSE equals
  the S6 formula applied to that file's own predictions and targets (worst
  relative difference 2.6e-15).
- At the G2-N1 cell (5 nodes, 0.1 nA, M = 2) version 2 is lower at every
  order, for example NARMA-2 test 0.356722 against 0.421309, NARMA-10 test
  0.598446 against 0.623601.

This is consistent with version 2's results having been produced by version
2's code (squared-state features) and version 1's results by a feature set
without squares; which code produced each file stays UNVERIFIED without
MATLAB (discrepancy 15). G2-N1 tests it numerically (section 7.1 items 1 and
10).

## 2. What the paper claims

- **[paper] S4 eq. 18**: one membrane, dV/dt = (h / (eps A)) (I_inj - V G_u N_a A).
- **[paper] S4 eq. 19**: alpha(V) = 1 / (0.001 e^(2.22 V)).
- **[paper] S4 eq. 20**: N_ss(V, c) = 0.002 e^(689.4 c V), c in micromolar.
- **[paper] S4 eq. 21**: dN_a/dt = alpha(V) N_a (1 - (N_a / N_ss(V))^Z), Z a shape parameter (no value given).
- **[paper] S4 network form**: KCL per node with membrane area matrix A; initial conditions "x_i(t = 0) = 0 for all nodes and membranes (zero initial voltages and pore densities)"; integrated with MATLAB ode15s (sm.txt:567-572).
- **[paper] NARMA (main text)**: u(k) uniform in [0, 0.5], mapped to I_inj in [I_min, I_max], binary mask (1 keeps the encoding, 0 flips the range), mask segment theta = 1 ms; 3000 training and 3000 testing inputs, last K = 800 kept; states = node voltages sampled at the end of each pulse; features augmented with squared states. Best configuration N = 50 nodes, M = 3; connected NARMA-2 train/test NRMSE 0.0103 / 0.0124, NARMA-10 0.236 / 0.327; parallel NARMA-2 0.433 / 0.458, NARMA-10 0.546 / 0.627.
- **[paper] Lorenz (main text)**: 1 Lorenz time unit = 1 s; inputs linearly scaled to -50 to +50 nA; branched network, three injection nodes, one grounded central node; ridge readout mapping states to the next Lorenz state; 43-node test NRMSE x, y, z = 0.071, 0.183, 0.174; 13- and 25-node networks obtained by removing outer layers.
- **[paper] S6**: NMSE = sum |y_ref - y_pred|^2 / sum |y_ref - mean(y_ref)|^2, NRMSE = sqrt(NMSE).

## 3. What the code does

### 3.1 Model (both tasks)

State: node voltages V (V), then one pore density N_ij per directed membrane
side (i -> j), dimensionless count per the code's units.

- Capacitance: **[code] H:228, H:354-356; L:26-27, L:72** M V' = B with
  M = (eps / h_m) (diag(rowsum(Aarea)) - Aarea), Aarea = adjacency * 5e-8 m^2
  (all membranes the same area A = 5e-8 m^2; `conmat` entries are 0/1
  **[archive]**). Ground rows and columns removed and V'_ground = 0
  (**H:388-403** `solveWithMultipleKnown`, **L:74-76, L:320-326** Cholesky).
  Injected current at a ground node is dropped.
- Currents: **[code] H:358-361, L:315-319** B_i = I_i + sum_j G_u A (N_ij + N_ji) (V_j - V_i), G_u = 5e-8 S (**H:249, L:26**).
- Pore kinetics: **[code] H:304-319, L:331-338** with dV = V_i - V_j and the
  parameters of the side's start node i:
  dN_ij/dt = (1e4 / (tau0 e^(dV / v_tau))) N_ij (1 - (N_ij / (N0(c_i) e^(dV / v_e)))^Z),
  N0(c) = 0.002 e^(3.7919 c) (**H:236, L:58**), Z = 0.00015 (**H:317, L:335**),
  tau0 = 0.001 s (**H:237, L:56**), v_e = 0.0055 V (**H:239, L:59**),
  v_tau = 0.045 V NARMA (**H:238**), 0.0454 V Lorenz (**L:57**).
  The result is passed through `real()`; NARMA replaces a NaN derivative
  by 0 (**H:378-380**).
- Constants: h_m = 4e-9 m (**H:247, L:35**); eps = 8.85e-12 F/m NARMA
  (**H:248**), 2.1 * 8.85e-12 F/m Lorenz (**L:34**).
- Concentrations: repeating 1, 2, 3 uM by node number (**H:231-234, L:52-54**);
  0-based node i has c = 1 + (i mod 3).
- Initial conditions: V = 0. NARMA N_ij(0) = N0(c_i) (**H:264-281**; the
  `inex == 2` branch is never taken because every `inex` entry of the four
  Length files is 1 **[archive]**). Lorenz N_ij(0) = 1 (**L:68, L:166-167**).
- Solver: ode15s, RelTol 1e-6, AbsTol 1e-8 (**H:287-290**; **L:83, L:102**
  with a Jacobian sparsity pattern). Binary64 throughout (MATLAB default).
  Output at the solver's own steps (two-element time span, **H:288, L:82**).

### 3.2 Topologies

- NARMA chains, `Length{5,10,20,50}.mat` **[archive]**: `conmat` 0/1, `ground`, `inex`.
  Length5/10/20 are the path 1-2-...-L grounded at L. Length50 is one path
  with permuted node numbers, 49 edges, leaves 1 and 2, ground 50 (an
  interior node). All four are in `res_fixtures.h` or built by
  `omega_res_topology_chain`.
- Lorenz lattices **[code]**: neighbour tables L:212-254 (43 nodes, ground 21,
  inputs 1, 28, 42, L:30-31), 13Nodes.m:211-223 (ground 8, inputs 1, 2, 13),
  25Nodes.m:211-235 (ground 14, inputs 1, 2, 25). The tables are
  symmetrized with max(A, A') and the diagonal zeroed. Counted after
  symmetrization: 13 nodes 21 edges, 25 nodes 51 edges, 43 nodes 99 edges;
  centre degree 6, every injection node degree 1 **[measured]** (tests
  `test_reference_lattices`).

### 3.3 Inputs

- NARMA, condition 7 (**H:55, H:583-603**): rng(2), u = rand(1, 1000) * 0.5
  for train and test (**H:458-466**); I = u * I_hi / 0.5 with I_lo = 0
  (**H:468**); each sample lasts M sub-pulses of pw = 1 ms (**H:49**), each
  sub-pulse 10 rows at dt = 0.1 ms (**H:462-463**); node n gets mask
  `superSequence(M, n)` = a cyclic slice of the binary de Bruijn sequence
  B(2, ceil(log2 M)) starting at randi(2^n) after rng(n) (**H:404-423**); mask
  bit 1 -> I, bit 0 -> I_hi - I (`inverter`, **H:297-301**). Currents are
  linearly interpolated between the 0.1 ms rows (**H:328**). Grid: I_hi in
  {0.1, 0.25, 0.5, 1} nA, M in {2, 3, 4, 5}, L in {5, 10, 20, 50} (**H:17-19**).
- Lorenz (**L:17, L:41-49, L:150-163**): X, t from LorenzData1.mat (train,
  2105 samples, t in [0, 40], non-uniform) and LorenzData2.mat (test, 2185
  samples, starts at 1.01) **[archive]**. I_in1 = (x * 1.05e-10 + 1e-9) / 10,
  I_in2 = (y * 1.05e-10 + 1e-9) / 10, I_in3 = z * 2.05e-10 / 10, linear
  interpolation on the non-uniform t (`griddedInterpolant`), Lorenz time
  used as seconds.

### 3.4 Sampling, features, readout, error

- NARMA sampling (**H:616-640**): row i (1-based) is V of every node at
  t = i * pw * M + k * pw, k = 1..M, linearly interpolated in the ode15s
  output; a row with any time past the end is set to zero. Features
  (**H:163-177**): [zeros row; sampled rows], L * M voltage columns plus the
  input u as last column, no squared terms. NARMA-o targets (**H:110-142**):
  y(c) = 0.3 y(c-1) + 0.05 y(c-1) sum_{i=0}^{o-1} y(c-1-i) + 1.5 u(c-o) u(c-1) + 0.1.
  Readout (**H:180-201**): rows 100 .. 995 (896 samples), `fitlm` ordinary
  least squares with intercept, no ridge, orders 2..10, NRMSE by
  `goodnessOfFit(pred, ref, 'NRMSE')`.
- Lorenz (**L:106-139, L:173-183**): features = all node voltages at every
  ode15s output time (ground column pinned to 0), range-scaled to [0, 1] with
  the training min and max, bias column prepended (**L:344-360**); targets =
  the Lorenz signal interpolated at the same times (**L:118, L:179**);
  multi-output ridge with unpenalized bias, W = (Phi'Phi + lambda P) \ Phi'Y
  (**L:362-369**); lambda from linspace(0.033, 0.035, 101) by 5-fold blocked
  cross-validation, folds round(linspace(0, n, 6)), score = mean of the three
  NRMSEs (**L:121-123, L:371-405**).
- NRMSE: `goodnessOfFit` with 'NRMSE' is ||ref - pred|| / ||ref - mean(ref)||,
  the same as S6 eq. 25. UNVERIFIED (85%): taken from the MathWorks
  description, not checked against MATLAB.

### 3.5 Archived results usable as reference

- **[archive]** HyperParameterOptimizationVariablesToPlot.mat: NRMSEtrain and
  NRMSEtest (4 x 4 x 4 x 10: L, I_hi, M, order), predictionTrainCell,
  predictionTestCell, yTrainCell, yTestCell (896 samples each). Test NRMSE at
  L = 50, I_hi = 0.1 nA, M = 3: NARMA-2 0.0124, NARMA-10 0.3269 (matches the
  paper's 0.0124 and 0.327).
- **[archive]** trainSignal.mat / testSignal.mat (connected): uTrain, uTest,
  sequenceTrain/Test, Itrain/Itest 20000 x 5, trainsignaltime 0..1.9999 s:
  the inputs of grid cell L = 5, I_hi = 0.1 nA, M = 2.
- **[archive]** Figure5_Data{,13Nodes,25Nodes}.mat: Ytrain, Ytest, predicted
  X/Y/Z (train and test) at the ode15s output times, timeSolutionTrain/Test
  (43 nodes: 55134 / 55425 points), nrmseTraining, nrmseTest. 43-node test
  NRMSE 0.0707, 0.1825, 0.1737; train 0.0607, 0.1669, 0.1326. 13 nodes test
  0.1245, 0.2435, 0.2223. 25 nodes test 0.1099, 0.1556, 0.2172.
- **[archive]** ParallelReservoirDataToPlot.mat: parallel NARMA NRMSE (test
  NARMA-2 0.4577, NARMA-10 0.6273).

## 4. Discrepancies

Paper against code:

1. alpha: paper 1 / (0.001 e^(2.22 V)); code 1e4 / (0.001 e^(dV / 0.045)), that is e^(22.2 dV) and an extra factor 1e4.
2. N_ss: paper 0.002 e^(689.4 c V) (one exponent, product of c and V; 689.4 = 3.7919 / 0.0055); code 0.002 e^(3.7919 c) e^(dV / 0.0055) (sum of exponents). Different functions of c and V.
3. Initial conditions: paper zero pore densities; code N0(c) (NARMA) or 1 (Lorenz). N = 0 is a fixed point of eq. 21, so the paper's stated initial state would never move.
4. Z: no value in the paper; code 0.00015.
5. NARMA data size: paper 3000 + 3000 inputs, last 800 kept; code 1000 + 1000, rows 100..995 (896).
6. NARMA features: paper adds squared states; code has none (voltages plus u).
7. NARMA sampling: paper "at the end of each pulse"; code row i samples the sub-pulses of input i + 1 (query times i * pw * M + k * pw with 1-based i, the comment at H:625 states the 0-based formula).
8. NARMA best configuration: the paper calls N = 50, M = 3 optimal; in the archived grid M = 5 is better for NARMA-2 (0.0058) and NARMA-3 (0.0182).
9. Lorenz current range: paper -50 to +50 nA; code, over the archived training and test data, -0.091 to +0.306 nA (x node), -0.158 to +0.386 nA (y node), +0.020 to +0.982 nA (z node).
10. Lorenz target: paper "next Lorenz state"; code fits the signal at the same time as the state (L:118, L:179).
11. Permittivity: the code uses eps = 8.85e-12 for NARMA and 2.1 * 8.85e-12 for Lorenz; the paper gives no value.

Code against its own archive:

12. LorenzDataGenerator.m writes `Lorenz63_Data.mat` on a 266485-point uniform grid from (1, 1, 1); the solvers load LorenzData1.mat (2105 non-uniform points) and LorenzData2.mat (starts at 1.01). The archived Lorenz data were not produced by the archived generator.
13. Signal file names: the code saves `Trainsignal.mat` (H:611), the archive holds `trainSignal.mat`.
14. The 25-node builder is named `buildAdjacencyMatrix43` (25Nodes.m:208-209).
15. Every `.m` file carries "Added comments/documentation only; no functional changes" and "arithmetic update" markers: the archived results may come from an earlier version of the code. Not checkable without MATLAB.
16. Silent repairs in the reference: `real()` of the pore derivative, NaN derivative replaced by 0 (H:378-380), NaN sample rows replaced by 0 (H:633-635). Omega refuses these cases instead.

## 5. Reproduction of the reference: NOT_RUN

`command -v matlab`, `command -v octave` and `command -v octave-cli` all
return nothing on this machine; nothing was installed. Running the reference
also needs `fitlm` (Statistics and Machine Learning Toolbox) and
`goodnessOfFit` (System Identification Toolbox, UNVERIFIED 85%). This is a
decision for Drake (section 9). Gate 2 below is designed to run without
MATLAB against the archived outputs.

## 6. The Omega implementation (Gate 1)

Files: `src/reservoir/omega_reservoir.{h,c}` (simulator),
`src/reservoir/omega_reservoir_readout.{h,c}` (ridge and NRMSE),
`tests/reservoir/` (tests, binary64 oracle, fixtures), `mk/reservoir.mk`.

Same model as section 3.1, written in Omega's own form:

- Separate types: topology (canonical sorted edges, degree per node,
  grounds, every component grounded), parameters, integrator, built
  configuration (factored Laplacian, derived constants, identity), state,
  input table, workspace, run receipt, readout.
- State y = ln N per side (exact change of variables): y' = alpha (1 - e^w),
  w = Z (y - ln N_ss). Densities stay positive under an explicit method, and
  1 - e^w is evaluated without cancellation (series for |w| <= 0.5).
- binary32, round to nearest, no flush to zero, `-ffp-contract=off`, all
  exp/log/div/sqrt through `omega_math_*`; declared update order in the
  header; deterministic, bit-identical across runs.
- Classical RK4, fixed step h, integer time; compensated (TwoSum) state update.
- Input: piecewise-linear current on a uniform knot grid (the NARMA
  reference grid exactly: 0.1 ms rows).
- Stiffness guard: refuses a step with h * lambda > 2 (lambda bounds the
  fastest decay rate).
- Refusals with error codes for invalid topology or parameters, NaN or
  infinity, a membrane voltage |V_a - V_b| > 1 V (node voltages: finite
  only; section 12), ln N outside [-80, 80], |I| > 1 uA, time past the
  last knot, state of another configuration, aliasing. Membranes above
  0.3 V are counted as physically implausible and recorded, not refused.
- Caller-owned fixed-size buffers (up to 64 nodes, degree 6), no
  allocation (checked by `reservoir-purity` with `nm -u`), no OmegaTensor use.
- Content identity: SHA-256 with domain separation through `src/sha256.c`
  (config id, input digest, state digest, run id). No new crypto.

Implemented topologies: those the reference uses, chains and the hexagonal
lattices (any graph of degree <= 6 with a ground in every component).
Not implemented: the parallel reservoir (voltage-clamped membranes,
ParallelReservoirNARMA.m), per-membrane areas other than one shared value.

### 6.1 Where Omega differs from the reference method, and the evidence

The reference integrates in binary64 with ode15s (variable order NDF/BDF,
RelTol 1e-6, AbsTol 1e-8). Omega uses binary32 RK4 at a fixed step. The
check: an independent binary64 oracle (`tests/reservoir/res_oracle64.c`:
N itself as state, libm `pow`/`exp`, LU with partial pivoting, adaptive
Dormand-Prince 5(4) restarted at each knot) run at a tight tolerance
(rtol 1e-10, atol 1e-14) as truth and at the reference's tolerances
(rtol 1e-6, atol 1e-8) as the matched-tolerance run. The oracle is not
ode15s (not the same method); it matches the tolerance and precision.

**[measured]** maximum |V_omega - V_truth| over all nodes and knots (plain
build; the ASan build runs shorter scenarios with the same pattern):

| Scenario | max abs V | h = 1e-4 s | 5e-5 | 2e-5 | 1e-5 | 5e-6 | binary64 at ref. tol. |
|---|---|---|---|---|---|---|---|
| 1 membrane, 0.1 nA, 200 ms | 85.9 mV | 1.43e-8 V | 1.47e-8 | 1.47e-8 | 1.47e-8 | 1.47e-8 | 3.9e-16 |
| chain 5, masked NARMA-style drive, 200 ms | 289 mV | 3.84e-8 | 3.84e-8 | 3.58e-8 | 3.58e-8 | 3.58e-8 | 4.0e-12 |
| hex 13, Lorenz constants, sine drive, 100 ms | 86.1 mV | 2.38e-8 | 2.51e-8 | 2.60e-8 | 2.60e-8 | 2.60e-8 | 6.1e-16 |

Reading: the binary32 error is 2 to 5 ulps of V (at most 3e-7 of the
largest voltage) and flat in h, so truncation error is below binary32
resolution over the whole range; the reference tolerances are tight enough
that the matched-tolerance run sits 4 orders of magnitude closer to truth
than binary32 can. Before the compensated update the error grew as h
shrank (one membrane: 2.1e-6 V at h = 1e-5 s, 6.0e-6 V at 5e-6 s) because
increments near equilibrium fell below half an ulp; the TwoSum carry
removed it (header comment, `omega_reservoir.h`).

Step choice: h = 1e-5 s with 10 steps per 0.1 ms knot is the declared
Gate 2 step. Accuracy would allow 1e-4 s; the smaller step keeps a 10x
margin to the stability limit for stiffer states (larger voltage swings
raise alpha by e^(|dV| / v_tau)), and the guard refuses a step rather
than run unstable.

Other deliberate differences: no `real()` and no NaN-to-0 repair (refusal
instead); state variable ln N instead of N; current at a ground node
ignored (as the reference does).

### 6.2 Gate 1 tests and results

`make PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-reservoir`:
purity check, then 656 checks in a plain and an ASan/UBSan build: topology
validation and canonical form, the reference lattices and the 50-chain
(edge counts, degree histograms, boundary degree, grounds, injection nodes),
parameter refusals, identity (order-independent, -0 = +0, one ulp in any
field changes it), zero-input fixed point, closed-form decay of ln N,
charging V = I t / C and I t / (2C), saturation at the bisected V*,
connected response and an exactly-at-rest disconnected component, the
43-node lattice smoke run, bit-identical reproducibility (two runs,
step-by-step against `advance`, dirty all-NaN workspace), digests and run
receipts, every refusal path, oracle agreement (contract: every step size
within 1e-6 of max |V|; reference tolerances within 1e-9), ridge readout
and S6 NRMSE (sqrt(1/5) case). Result: all pass in both builds.

Mutation controls (scratch copies outside the repo, test run against each):
uncompensated update -> 13 failures; RK4 weight 2 -> 1.9 -> 25 failures;
stiffness guard disabled -> 1 failure. `make test-numeric-cpu` passes
(21 of 21, 5 declared chip-only skips).

## 7. Gate 2 comparison contract (declared before any comparison is run)

All comparisons use the archived files of section 1 as inputs and as
reference outputs, the Omega simulator at h = 1e-5 s, K = 10, and the
binary64 readout. Inputs are stored in binary32 knots (relative rounding
6e-8, declared). Tolerances are engineering choices fixed here; a failure
is investigated, never answered by loosening them.

**G2-N1, NARMA, executable now.** Grid cell L = 5 (Length5.mat, path
grounded at node 5), I_hi = 0.1 nA, M = 2, condition 7.
- Inputs: archived Itrain and Itest (20000 x 5) used as the knot table,
  20000 knots at 0.1 ms; uTrain, uTest archived.
- Simulation: V = 0, N(0) = N0(c_i) per side, to t = 1.9999 s.
- Sampling and features exactly as section 3.4 (including the one-sample
  offset, zeroed rows past the end, the zero first row and the appended u).
- Readout: least squares with intercept (lambda = 0) on rows 100..995 for
  orders 2..10.
- Metrics and acceptance: (a) NARMA targets against yTrainCell /
  yTestCell{1,1,1,o}: max abs difference <= 1e-12; (b) predictions against
  predictionTrainCell / predictionTestCell{1,1,1,o}: RMS difference <= 1 %
  of the target standard deviation; (c) NRMSE against NRMSEtrain /
  NRMSEtest(1,1,1,o): |difference| <= max(0.005, 5 % of the reference
  value). All nine orders must pass.

**G2-L1, Lorenz, executable now.** 43, 25 and 13 nodes.
- Inputs: archived LorenzData1 (train) and LorenzData2 (test); currents as
  section 3.3, resampled onto a uniform 0.1 ms knot grid (400001 knots).
  This resampling is an approximation of the reference's interpolation on
  the non-uniform t; its maximum current error is computed and reported
  before the comparison.
- Simulation: V = 0, N(0) = 1, t in [0, 40] s.
- Sampling: V at the archived timeSolutionTrain / timeSolutionTest points
  (linear interpolation between Omega steps 1e-5 s apart); targets by
  linear interpolation of X at those times.
- Readout: range scaling with training min/max, bias, ridge with
  unpenalized bias, lambda by the 5-fold blocked CV over
  linspace(0.033, 0.035, 101) as section 3.4; the chosen lambda is
  reported (it is not archived).
- Metrics and acceptance: predictions against archived predictedX/Y/Z
  (train and test): RMS difference <= 1 % of the target standard deviation
  per variable; NRMSE against nrmseTraining / nrmseTest: |difference| <=
  max(0.005, 5 % of the reference value).

### 7.1 Amendments, declared 2026-10-14 before any Gate 2 comparison was run

Committed before the comparison tool was first run against the archive
(the commit order on the branch shows it). They clarify or tighten the
contract above; none loosens a tolerance.

1. **Second NARMA feature set (N1b).** Zenodo version 2 of the same concept
   (section 1.1) changes HyperParameterTuningOptimized.m to append squared
   states: features [V, V.^2, u] (v2 lines 168, 176-177). G2-N1 is therefore
   run twice with identical inputs, simulation and acceptance criteria:
   **N1a** = v1 code features (V, u), as contracted above; **N1b** = v2 code
   features (V, V^2, u). Each gets its own PASS or FAIL. Neither substitutes
   for the other. If exactly one passes, that is evidence about which code
   produced the archived v1 results (discrepancy 15), not a choice of the
   better number.
2. Feature columns that are identically zero over the fit rows (the ground
   node's voltage and its square) are removed before the fit. fitlm drops
   them as rank deficient; the predictions do not depend on them.
3. PASS or FAIL uses `omega_res_readout_fit` (lambda = 0, LDL of the normal
   matrix). A Householder QR solve of the same least-squares problem (the
   method family of fitlm) is reported as a diagnostic only.
4. "Target standard deviation" in criterion (b) of both cases is the
   population standard deviation of the archived reference targets of that
   split and output.
5. State traces: the v1 archive holds no reservoir state trajectories, only
   inputs, targets, predictions and NRMSE. A state-trace comparison against
   the authors' code is BLOCKED. The intermediate quantities compared are the
   input currents (knot rounding and resampling error), the targets and the
   predictions. Omega's state traces were compared with the independent
   binary64 oracle in Gate 1 (section 6.1).
6. Negative controls, expected to FAIL: **NC-N** = N1a with the permittivity
   multiplied by 2.1 (the Lorenz value); **NC-L** = G2-L1 on 13 nodes with
   the NARMA permittivity 8.85e-12 F/m. A control that passes is reported as
   a finding that the contract cannot see that error.
7. Positive controls: each archived NRMSE is recomputed from the archived
   predictions and targets with the S6 formula (checks the metric
   definition); for Lorenz, archived Ytrain/Ytest must equal LorenzData1/2 X.
8. Lorenz time mapping: an archived sample time t maps to step position
   t / 1e-5. The binary32 step is fl32(1e-5) = 9.99999975e-6 s, so Omega's
   integration time runs 2.5e-8 relative short of nominal (1 us over 40 s);
   declared, not corrected.
9. Lorenz knot grid: 400001 knots, k * 1e-4 s for k = 0..400000 (archived t
   runs from 0 to 40 exactly).
10. **Added before the first run, after the version 2 results file was read
    (section 1.1): N1b-v2.** N1b's Omega predictions are also compared,
    under the same criteria (a), (b) and (c), with the version 2 archived
    results (the sha256-pinned 14,525,399-byte file). N1a and N1b against
    version 1 stay as declared in item 1. This adds a case and loosens
    nothing. The pinned reference remains version 1; N1b-v2 is reported as
    a separate row, and a PASS there is agreement with version 2's code and
    outputs, not with the version the paper cites.

**BLOCKED or out of scope**
- NARMA grid cells other than (5, 0.1 nA, 2): their masks come from
  MATLAB's rng(n) / randi, which is not reimplemented. Needs MATLAB, or a
  verified reimplementation (the only check available offline is the 5
  mask bits recoverable from the archived cell).
- The parallel reservoir (S5): not implemented in Omega.
- Rerunning the authors' MATLAB, and whether the archived results came from
  the archived code (discrepancy 15): needs MATLAB.
- Lorenz data generation (discrepancy 12): the archived LorenzData files are
  taken as given inputs.
- Physical droplets and experiments: outside any numerical comparison.

## 8. What a receipt proves

`omega_res_run_receipt` binds config id (topology, parameters, step, and the
numeric policy string), input digest, step count and final-state digest.
It proves that this configuration, this input and this numeric policy
produced this final state on Omega's CPU tier, bit for bit, and lets anyone
rerun and compare. Together with the test results above it records
agreement with the binary64 oracle on the stated scenarios. Agreement with
the published reference is what Gate 2 adds; section 11.7 states what its receipt proves and what it does not. No receipt or test here says
the model describes real droplets: that is the authors' physical claim, and
section 4 lists where their own paper and code disagree about the model.

## 9. Decisions for Drake

Decided by Drake on 2026-10-14:

1. MATLAB access (section 5): no MATLAB or Octave is installed. Gate 2
   compares against archived outputs only: the one NARMA grid cell whose
   inputs are archived and the three Lorenz networks. The authors' code
   stays NOT_RUN and the other 63 NARMA cells stay BLOCKED.
2. Where paper and code disagree (discrepancies 1-3, 9-10), the canonical
   model is the authors' code, because the archived numbers come from the
   code. The discrepancy list (section 4) is kept.

Decided by the orchestrator on 2026-10-14, under Drake's standing
delegation, after the first Gate 2 run (receipt `6d1c5abb...`):

3. The simulator's 1 V refusal applies to each membrane voltage
   |V_a - V_b|, not to the node voltages. Node voltages are referenced to
   the common ground and add up along a chain; the pores see only the
   voltage across their own membrane. 1 V is a numerical guard against
   blowups, not physics. The paper says rupture can occur "when voltages
   exceed ~200 to 300 mV", so a membrane above 0.3 V is flagged as
   physically implausible and recorded, never refused. Node voltages are
   checked for finiteness only. Full rule, reasoning and evidence:
   section 12. Under this rule the 50-node NARMA chain is still refused,
   now by the stiffness guard (section 11.2).

Open, raised by the rerun (section 11.2), not decided:

4. Whether to run the 50-node chain at a step smaller than the declared
   h = 1e-5 s. A smaller step raises the decay rate the guard accepts
   (h * lambda <= 2); whether the run would then complete is UNVERIFIED.
   Any such run would be post hoc and labeled so. None has been run.

## 10. Gate 2 benchmark protocol (declared 2026-10-14, before any benchmark run)

Omega's own benchmark, on data from Omega's own generator. It is not a
reproduction of the paper's numbers (those are section 7). The same
simulator, step (h = 1e-5 s, K = 10) and readout serve every reservoir.

**Generator.** splitmix64 (`tools/reservoir/res_common.c`), one stream per
(seed, purpose). Seeds 1, 2, 3, 4, 5. Every seed is reported, including
failures.

**Readout (all models alike).** Features standardized with the training
mean and population standard deviation; ridge with an unpenalized bias
(`omega_res_readout_fit`); lambda from {1e-6, 1e-5, ..., 1e2} (9 values) by
5-fold blocked cross-validation on the training rows only (contiguous folds,
score = mean validation NRMSE); refit on all training rows; the test run is
touched once, for the reported test NRMSE (S6 formula).

**NARMA-2, -5, -10.** u ~ U[0, 0.5) i.i.d.; 2000 training and 1000 test
inputs, separate runs from rest; targets by the reference recursion
(section 3.4); the first 100 targets of each run are washout. Drive as the
reference condition 7: M = 3 sub-pulses of 1 ms, I_hi = 0.1 nA, mask bit 1
-> I = 2 u I_hi, bit 0 -> I_hi - I; per-node masks are cyclic slices of the
de Bruijn sequence 0011 with a start drawn from the generator. Sampling: the
voltages at the end of each sub-pulse of input c - 1, plus u(c - 1), are the
features for target y(c) (the paper's "end of each pulse"; not the code's
one-sample offset, discrepancy 7).
- **RC-connected**: the 50-node reference chain (Length50.mat), NARMA
  constants, 49 driven nodes, 148 features.
- **RC-disconnected**: the same 49 driven nodes, concentrations, masks and
  currents; every inter-droplet membrane removed and each driven node given
  one membrane to a grounded droplet of its own concentration (grounds shared
  by at most six driven nodes; a grounded droplet is held at 0 V, so this
  couples nothing). 58 nodes, 148 features. This is the coupling ablation in
  the same current-driven model. The paper's parallel reservoir (S5,
  ParallelReservoirNARMA.m) is a different, voltage-clamped model with other
  kinetics and is not implemented.
- **RC with squared states**: NOT_RUN for NARMA (295 features, above the
  readout bound of 256).
- **BL-linear**: ridge on the delay line u(c-1) .. u(c-12) (12 features).
- **BL-quadratic**: the same plus every product u(c-i) u(c-j), i <= j
  (90 features).

**Lorenz63.** sigma 10, rho 28, beta 8/3, binary64 RK4 at dt = 1e-4 (1
Lorenz time unit = 1 s, as the paper). Initial state from the generator
(x, y in [-10, 10), z in [10, 40)), 20 time units discarded, then 20 time
units recorded; training and test are separate trajectories. Currents into
injection nodes 1, 28, 42 of the 43-node lattice as the reference code
(section 3.3); the 1e-4 s trajectory points are the knots. Sampling every
10 ms; the first 2 s are washout; rows 200 .. 1975 (1776 per run) for every
task. Tasks: **T0** target X(t) (what the reference code fits,
discrepancy 10), **T1** X(t + 0.01 s) (the paper's "next state"), **T25**
X(t + 0.25 s). One ridge fit per task with three outputs.
- **RC-connected**: 43-node lattice, Lorenz constants, the 42 non-ground
  voltages (RC-lin) or those plus their squares (RC-sq, 84 features).
- **RC-disconnected**: the three injection nodes, each with one membrane to
  a grounded droplet of its own concentration (5 nodes); 3 or 6 features.
  Without coupling the other 40 droplets receive nothing, so this ablation
  also has far fewer features; reported as such.
- **BL-linear**: the input X at t, t - 10 ms, .., t - 40 ms (15 features);
  **BL-quadratic**: plus all 120 pairwise products (135 features). On T0 the
  baselines see the target itself, so they are expected to be near exact:
  the reference Lorenz task is a reconstruction of the current input.

**Reported per model, task and seed:** test NRMSE (per output for Lorenz),
training NRMSE, chosen lambda, CV score, simulator status and run receipt.
Summaries: mean, sample standard deviation, min and max over seeds, and the
paired difference connected minus disconnected and connected minus best
baseline, with the number of seeds where connected wins. No significance
test is claimed with five seeds.

**Performance (task e).** On an idle machine (quiet flag checked), one
process pinned to one core: per-step latency of `omega_res_step` for the
5-node chain (G2-N1), the 50-node chain and the 43-node lattice, 5
repetitions of 40,000 timed steps after 10,000 warm-up steps; p50, p95,
p99, max; throughput in steps/s and simulated seconds per wall second; CPU
time over wall time from getrusage; peak resident memory of the process
(ru_maxrss) and the fixed sizes of the simulator structs; host, core,
kernel, compiler, flags and commit. Energy and power: NOT_RUN (not
instrumented). No efficiency claim.

**AIEN telemetry workload (task d).** Run only if real telemetry with a
time-ordered train/validation/test split exists in the repository or on
this machine; otherwise NOT_RUN with the reason.

### 10.1 Amendment after the first NARMA benchmark run (post hoc, labeled)

The declared RC-connected NARMA network (the 50-node chain, 49 driven nodes
at up to 0.1 nA each) is refused by the simulator in every seed, training
and test alike: a node voltage leaves the model's declared range
(|V| <= 1 V, `OMEGA_RES_V_LIMIT`) after about 3.5 ms (step 347 to 359). The
currents of 49 nodes add up along the chain toward its single ground. That
refusal stands as the declared result and is reported as such.

Added after seeing it, and marked post hoc wherever it is reported: the
same NARMA protocol on the 5-node reference chain (`Length5.mat`, the G2-N1
network: 4 driven nodes, ground node 5, 13 features) and its disconnected
ablation built the same way (4 driven nodes and 3 grounded droplets). Its
masks are the first four of the 49 drawn for that seed. Nothing else
changes, and no network size or current was searched. The 50-node runs are
kept in the output.

Update after the voltage guard decision (section 12; the text above is
kept as written): the node-voltage bound named above (`OMEGA_RES_V_LIMIT`)
no longer exists. Under the membrane rule (`OMEGA_RES_VM_LIMIT`) the same
50-node chain is refused later, by the stiffness guard, at step 1,312 to
1,465 (section 11.2). The post hoc 5-node runs stay as declared here.

## 11. Gate 2 results

All numbers in this section come from one receipt,
`evidence/RESERVOIR/0043b137712388079be90ab408afc6a7bffc93146a8fbabd530fc01fe2dae9e4.json`
**[measured]**, written by `make qualify-reservoir` on commit
`48c6ed7175909543be6098c1866d179c27c61c1d` (tree `fb52582c`), clean tree
(`tree_dirty` false), unchanged during the run (`tree_stable_during_run`
true), 2026-10-14 16:49:49 to 16:54:30 UTC, host NVIDIA DGX Spark (Linux
7.0.0-1019-nvidia aarch64, 20 cores), gcc 13.3.0, CPU only. That commit
carries the membrane-voltage guard of section 12. The receipt status is
RECORDED: every step ran, the tree was clean and stable, the 22 archive
pins matched, the unit tests passed (681 + 681 simulator checks, 67 + 52
reader checks, plain and ASan/UBSan; `unit_tests`) and both negative
controls failed as they must. RECORDED says the run is complete and clean;
it is not a PASS of Gate 2 (11.1). Later commits on the branch change only
this document, crumbs and evidence/.

History. The first Gate 2 receipt,
`evidence/RESERVOIR/6d1c5abbb524ca557989a1dc9a9eae53469892d096ab9ae4ed712e9d1eacedcb.json`
(commit `ac35a166`, 2026-10-14 15:55 to 16:00 UTC, 656 + 656 simulator
checks), ran with the earlier node-voltage bound. It stays in evidence/
unchanged. Comparing the two receipts field by field, with the per-run
`membranes` objects (new), `config_id`, `run_id`, `final_state_digest`,
the performance section (11.3) and the wall-clock fields (`wall_s`,
`cv.seconds`, `timing.note`) set aside, the only differences are the candidate
commit and tree, the start and finish times, the unit-test counts and log
digest, and the 50-node NARMA chain: its 10 runs and 15 fits changed from
ERR_RANGE to ERR_STIFF and its stop steps moved (11.2). Every verdict and
every other number is identical. The ids and digests differ because the
numeric policy string, which states the guard, is part of the config id
(`src/reservoir/omega_reservoir.c:200`), and the state digest binds the
config id (`omega_reservoir.c:607`).

### 11.1 Comparison with the archived reference outputs (section 7)

| Case | Verdict | Numbers |
|---|---|---|
| Archive pins (22 files) | PASS | 22 of 22 sha256 match |
| G2-N1 N1a (v1 features V, u) | **FAIL** | the declared solver refuses the fit at every order (ERR_SINGULAR; smallest pivot ratio of the design -8.9e-16, refusal at or below 1e-13); (a) targets match exactly (max difference 0); QR diagnostic: (b) prediction difference 2.5 % to 25 % of the target std at orders 2 to 10, train and test (bound 1 %), (c) NRMSE within bound at orders 2 to 9, out at order 10 (test 0.661 against 0.624) |
| G2-N1 N1b (v2 features V, V^2, u) against v1 outputs | **FAIL** | the declared solver refuses at every order; the QR diagnostic also finds the design rank deficient |
| G2-N1 N1b-v2 against v2 outputs | **FAIL** | as N1b |
| NC-N (N1a, permittivity x 2.1) | FAIL, as required | not discriminating here: the case it controls fails too |
| G2-L1 13 nodes | **PASS** | prediction difference at most 6e-6 of the target std; test NRMSE x, y, z 0.124515 / 0.243520 / 0.222342 against 0.124516 / 0.243520 / 0.222343 |
| G2-L1 25 nodes | **PASS** | at most 6.5e-5; test NRMSE 0.109878 / 0.155553 / 0.217199 against 0.109889 / 0.155561 / 0.217220 |
| G2-L1 43 nodes | **PASS** | at most 1.04e-4; test NRMSE 0.070690 / 0.182553 / 0.173749 against 0.070683 / 0.182511 / 0.173711 (paper: 0.071, 0.183, 0.174) |
| NC-L (13 nodes, NARMA permittivity) | FAIL, as required | prediction difference 12 % to 19 % of the target std |

Positive controls pass in every case: each archived NRMSE recomputes from
the archived predictions to 1e-9, and the archived Lorenz Ytrain and Ytest
equal LorenzData1 and LorenzData2. The Lorenz input resampling error is at
most 3.1e-5, 5.9e-5 and 4.8e-5 of the largest current (x, y, z channels),
reported before the comparison as declared. The chosen ridge lambdas are
0.035 (13 and 25 nodes) and 0.033 (43 nodes).

**Why G2-N1 fails (diagnosis, [archive] and [measured]).** In the archived
NARMA inputs the summed current into the four non-ground nodes is the same
at every knot (2e-10 A, to 1e-25 A). The chain therefore carries almost the
same total current at every moment, and the voltages are close to linear
combinations of one another: node 4's last and first sub-pulse samples
differ by 5.2e-9 V RMS, about one binary32 step at that voltage. The
least-squares problem is then ill conditioned. Perturbing Omega's features
by 1e-6 relative moves the QR predictions by 3 % to 11 % of the target std,
three to eleven times the 1 % tolerance. A binary32 simulator cannot be
expected to reproduce predictions that sensitive, and the declared solver
refuses instead of returning them. This is a finding about the reference
case, not tuned away: the tolerances stay as declared and G2-N1 stays FAIL.
Whether the authors' MATLAB, rerun, reproduces its own archived predictions
to 1 % is not checkable here (BLOCKED, needs MATLAB).

### 11.2 Benchmarks (section 10), test NRMSE, mean over seeds 1 to 5

NARMA (one output; lower is better):

| Model | NARMA-2 | NARMA-5 | NARMA-10 |
|---|---|---|---|
| RC-connected, 50-node chain (declared) | REFUSED, ERR_STIFF (5 of 5 seeds) | REFUSED | REFUSED |
| RC-disconnected, 50-node ablation | 0.0876 | 0.491 | 0.544 |
| BL-linear (12 delays) | 0.334 | 0.381 | 0.446 |
| BL-quadratic (90 features) | **0.0068** | **0.0189** | **0.299** |
| RC-connected, 5-node chain (post hoc, 10.1) | 0.230 | 0.486 | 0.590 |
| RC-disconnected, 5-node ablation (post hoc) | 0.220 | 0.534 | 0.567 |

- The declared connected 50-node chain is refused in every seed, training
  and test, now by the stiffness guard (ERR_STIFF), at step 1,312 to 1,465
  (13.1 to 14.7 ms of a 6 s training or 3 s test run;
  `results.bench-narma.per_seed[*].runs[0..1].stopped_at_step`). The 15
  RC-connected fits are ERR_STIFF with no numbers
  (`per_seed[*].fits[0..2]`). The paper's best NARMA configuration is this
  network size; the chain sums 49 injected currents toward one ground, and
  the reference drive is one-signed (every current in [0, I_hi]).
- Why it is refused **[code]**. Before every step the simulator bounds the
  fastest decay rate lambda as the larger of a conductance term
  (`lambda_unit * (N_ab + N_ba)`, `src/reservoir/omega_reservoir.c:374-379`)
  and a pore-kinetics term (`alpha * Z * e^w`, `omega_reservoir.c:351`), and
  refuses with ERR_STIFF when h * lambda > 2 (`omega_reservoir.c:476`,
  `OMEGA_RES_STABILITY_LIMIT`). The pore rate is
  alpha = rate_scale / (tau0 * e^(dV / v_tau)) (`omega_reservoir.c:347`),
  so it grows e-fold for every 45 mV (NARMA v_tau) that the voltage across a
  membrane goes negative on that side.
- Which term trips **[measured, diagnostic, not in the receipt]**: a
  throwaway build of the same commit with a print at the refusal, run on
  seeds 1 to 5, reproduced all ten stop steps of the receipt exactly. In
  every case the pore-kinetics term tripped, on the same membrane (edge 47,
  droplets 45 and 49 in Omega's zero-based numbering, the side owned by
  droplet 49) at a membrane voltage of -0.2202 to -0.2203 V: alpha 1.334e9 to 1.337e9 /s,
  times Z = 1.5e-4, gives lambda about 2.0e5 /s, so h * lambda = 2.0004 to
  2.0059. The conductance term was 242 to 469 /s, more than 400 times
  smaller.
- Voltages **[measured]** (`runs[*].membranes`): in the 10 refused runs the
  largest membrane voltage was 0.2202 to 0.2203 V (below the 0.3 V flag;
  inside the 200 to 300 mV band where the paper says rupture can begin)
  while node voltages reached 2.85 to 2.96 V. No membrane passed 0.3 V in
  any run of the receipt (section 12).
- History: the first receipt refused the same runs at step 316 to 377
  (3.2 to 3.8 ms) with ERR_RANGE, because a node voltage passed the old
  1 V node bound. Its text also cited a diagnostic in which the largest
  membrane voltage was about 0.83 V when a node voltage reached 1 V; that
  diagnostic is in no receipt, and the receipt measurement above (0.2203 V
  membrane at 2.96 V node, stiffness refusal) supersedes it. Whether the
  authors' ode15s runs of this chain stay below 0.3 V per membrane cannot
  be checked without MATLAB (UNVERIFIED).
- Post hoc 5-node chain: connected beats disconnected in 2 of 5 seeds
  (NARMA-2, mean difference +0.010), 5 of 5 (NARMA-5, -0.048) and 1 of 5
  (NARMA-10, +0.023). It never beats the best baseline (0 of 15 seed-task
  pairs). No significance test is claimed with five seeds.
- The paper reports connected NARMA-2 test 0.0124 and NARMA-10 0.327 on 50
  nodes with squared states. This benchmark does not reproduce or refute
  those numbers: its 50-node connected run is refused, squared states are
  NOT_RUN for NARMA (295 features above the readout bound of 256), and its
  masks and sampling follow section 10, not the reference code.

Lorenz63 (mean over x, y, z; connected = 43-node lattice, disconnected = 5
nodes):

| Model | T0 (X(t)) | T1 (X(t + 0.01 s)) | T25 (X(t + 0.25 s)) |
|---|---|---|---|
| RC-connected-lin (42 features) | 0.206 (sd 0.147) | 0.181 | 0.953 |
| RC-connected-sq (84) | 0.195 (sd 0.245) | 0.174 | 0.977 |
| RC-disconnected-lin (3) | 0.479 | 0.501 | 0.981 |
| RC-disconnected-sq (6) | 0.259 | 0.290 | 0.911 |
| BL-linear (15) | 1.1e-6 | 1.3e-4 | 0.670 |
| BL-quadratic (135) | **1.2e-7** | **9.0e-7** | **0.475** |

- Connected-lin beats disconnected-lin in 5 of 5 seeds on T0 and T1 and 4
  of 5 on T25; connected-sq beats disconnected-sq in 4, 4 and 3 of 5. The
  disconnected ablation also has far fewer features (section 10), so this
  is not a clean measure of coupling alone.
- No reservoir beats the best baseline in any seed or task. On T0 and T1
  the baselines see the input itself, so near-exact baseline scores are
  expected (section 10). On T25 the quadratic delay-line baseline is best
  (0.475 against 0.911 to 0.981).
- Every failure and null result above is kept in the receipt, seed by seed.

### 11.3 Performance (task e)

Quiet flag CLEAR, no GPU job running. One process pinned to CPU 5 (Cortex
X925), gcc 13.3.0 `-O2 -ffp-contract=off -fno-fast-math`, 5 repetitions of
40,000 timed steps after 10,000 warm-up steps; latency of one
`omega_res_step` (one RK4 step of 1e-5 s, including a 22 ns clock read):

| Network | p50 | p95 | p99 | steps/s | simulated s per wall s |
|---|---|---|---|---|---|
| 5-node chain (G2-N1) | 768 ns | 800 ns | 816 ns | 1.30 M | 13.0 |
| 50-node chain | 10.6 us | 10.7 us | 11.5 us | 94.4 k | 0.94 |
| 43-node lattice (99 membranes) | 14.8 us | 14.9 us | 16.5 us | 67.6 k | 0.68 |

Percentiles are pooled over the 5 repetitions
(`results.perf.networks[*].pooled`). CPU time over wall time 0.9999 (one
core busy). Peak resident memory of the process 8,528 KiB. History: the
first receipt measured 752 / 784 / 800 ns, 10.9 / 11.0 / 12.1 us and
14.9 / 15.1 / 16.9 us on the same host. The guard change moves the 1 V
check from each node to each membrane (`state_check`,
`src/reservoir/omega_reservoir.c:320-340`); the run-to-run spread of these
figures was not measured, so no speed change is claimed. Fixed struct sizes: config 19,948 B, state
3,632 B, workspace 13,360 B, readout workspace 538,672 B. Energy and power:
NOT_RUN (not instrumented). No efficiency claim is made, and nothing here is
GPU qualification.

### 11.4 Timing inputs

The NARMA drive cannot be used for the 50-node timing run: it is refused
after 1,312 to 1,465 steps (11.2; about 350 steps under the first
receipt's node bound), which would end the loop. The two chains are
timed instead with zero-mean pulses of 1 ms on every non-ground node,
uniform in +-0.1 nA divided by the number of driven nodes, drawn from the
benchmark generator (seed 1). These keep the 50-node chain running for the
whole timed loop (`results.perf.networks[1].status` OK in both receipts),
while one-signed pulses of the same size do not (diagnostic run under the
first receipt's node bound, not in any receipt). The 43-node lattice is timed with the reference
Lorenz currents of a trajectory. The simulator is fixed-step RK4 with no
adaptive step or iteration, so the timing input mainly has to stay in
range; any value-dependent cost inside the math library (exp) was not
measured.

### 11.5 BLOCKED and NOT_RUN

- BLOCKED: 63 of the 64 archived NARMA grid cells (masks from MATLAB's
  random stream, not reimplemented); a state-trace comparison with the
  authors' code (the v1 archive holds no state trajectories); whether the
  archived outputs came from the archived code (discrepancy 15); rerunning
  the authors' MATLAB; the paper's parallel reservoir (S5, a different,
  voltage-clamped model, not implemented).
- NOT_RUN: energy and power (not instrumented); RC with squared states on
  the 50-node NARMA chain (295 features above the readout bound of 256);
  the AIEN telemetry workload (task d). The only telemetry-like data in the
  repository are the ARGUS A/B benchmark rounds under `evidence/ARGUS`
  (at most about 600 rows of wall times per configuration). They have no
  forecasting target and no declared time-ordered train, validation and
  test split, so the condition of section 10 is not met.

### 11.6 Committed fixtures

`tests/reservoir/fixtures/Length5.mat` (sha256
`dc6e7914d216a21f664adcd4843e9f705616df47511879ba0bbeceb241b9b50a`, 2,130
B) and `tests/reservoir/fixtures/Length50.mat` (sha256
`ff4c9d14abb6f64b1ab9f301072dc67316b7fd59fe61d0b7632ee467a8c2aae3`, 3,324
B) are unmodified copies of the archived files of the same name (section
1), from Armendarez N., Mohamed A., Hasan M. S., Najem J., Zenodo
10.5281/zenodo.17296788, licensed CC-BY-4.0 (the `LICENSE` inside the
archive says MIT; both permit this with attribution). They let CI test the
`.mat` reader without the archive. No other archive file is committed.

### 11.7 What the Gate 2 receipt proves, and what it does not

It proves, for commit `48c6ed71` with gcc 13.3.0 on this host (receipt
`0043b137...`): agreement, under the tolerances declared before the runs,
between Omega's binary32 simulator with its binary64 readout and the
authors' archived outputs for the three Lorenz networks (13, 25 and 43
nodes; targets, predictions and NRMSE on the archived inputs); that G2-N1
does not meet the declared criteria, in all three variants, with the
diagnostics above; the benchmark and timing numbers of 11.2 and 11.3; that
under the membrane-voltage guard of section 12 every run that completed
kept every membrane below 0.3 V (0 implausible states in 76 recorded runs;
largest membrane voltage 0.0970 V in a completed run, 0.2203 V in a
refused one); and that the declared 50-node NARMA chain is refused by the
stiffness guard at the declared step in every seed. All verdicts and
numbers of the first receipt (`6d1c5abb...`, commit `ac35a166`) are
reproduced unchanged except the 50-node chain's refusal code and stop step
(11, history). Anyone with the archive can rerun
`make qualify-reservoir RES_ARCHIVE=...` and compare.

It does not prove: anything about real droplets or devices; agreement on
the BLOCKED cases or with the authors' code beyond its archived outputs;
that a reservoir beats a conventional baseline (it does not, in any seed
or task here); that the 50-node chain would run at a smaller step, or what
the authors' ode15s does on it; that 0.3 V is the right plausibility line
for real membranes (it is the upper end of the paper's 200 to 300 mV
rupture range); GPU qualification of any kind; energy, power or
efficiency; production readiness.

## 12. Voltage guard decision

Decided by the orchestrator on 2026-10-14, under Drake's standing
delegation, after the first Gate 2 run (receipt `6d1c5abb...`, section 11
history). Recorded as section 9, item 3. Implemented in commits `cc6db6cd`
and `48c6ed71`; header comment in `src/reservoir/omega_reservoir.h`.

**Rule.**

- Refused (ERR_RANGE): any membrane voltage |V_a - V_b| > 1 V
  (`OMEGA_RES_VM_LIMIT`), checked on every state (`state_check`,
  `src/reservoir/omega_reservoir.c:320-340`).
- Flagged, recorded, never refused: any membrane voltage above 0.3 V
  (`OMEGA_RES_VM_PLAUSIBLE`). Every bench and Gate 2 run records a
  `membranes` object: `states_observed`, `states_implausible` (states with
  at least one membrane above 0.3 V), `membranes_implausible`,
  `max_membrane_abs_v` and `max_node_abs_v`.
- Node voltages: finite only, no bound. Ground nodes stay exactly 0.
- Everything else is unchanged: the stiffness guard (h * lambda <= 2), ln N
  bounds, current bound, finiteness and the declared step.

**Reasoning.**

1. Node voltages are measured against the one common ground. Along a chain
   they add up membrane by membrane, so a long chain can carry node
   voltages of several volts while each membrane sees a fraction of a volt.
   A bound on the node voltage therefore depends on chain length, not on
   anything a membrane experiences.
2. The pore kinetics in the model see only the voltage across their own
   membrane (dV_ab = V_a - V_b in alpha and Nss, header comment). That is
   the physically meaningful quantity to guard.
3. 1 V is a numerical guard that catches blowups; it is not a physical
   limit. It sits far above the 200 to 300 mV the paper gives for rupture
   (point 4), so a membrane that reaches it means the numbers have run away.
4. The paper (main text) says the current-driven mode "minimizes the risk
   of electroporation or membrane rupture, which can occur when voltages
   exceed ∼200 to 300 mV". A membrane above 0.3 V is therefore outside what
   the authors consider physical, so it is flagged. It is recorded instead
   of refused: the model has no rupture term (section 3.1), and recording
   shows where a run leaves the physical range instead of hiding it.

**Tests.** `test_membrane_guard` (`tests/reservoir/test_reservoir.c`): a
30-node chain whose membranes neither conduct nor adapt, 10 nA into node 0.
The run must continue past a node voltage of 1 V (where the old guard
refused), flag every membrane once it passes 0.3 V, and refuse only when a
membrane passes 1 V. `make test-reservoir` runs it, plain and with
ASan/UBSan (681 checks each in the receipt's `unit_tests`).

**Effect on Gate 2 [measured], receipt `0043b137...`.**

- 76 runs record membrane statistics (16 Gate 2 runs, 20 Lorenz and 40
  NARMA benchmark runs). Across all of them `states_implausible` and
  `membranes_implausible` are 0: no membrane passed 0.3 V in any state.
- The 66 completed runs: largest membrane voltage 0.0970 V
  (`results.g2-lorenz-13-NC-L.runs[1].membranes.max_membrane_abs_v`),
  largest node voltage 0.349 V
  (`results.g2-lorenz-43.runs[0].membranes.max_node_abs_v`).
- The 10 refused runs (50-node NARMA chain, 5 seeds, train and test):
  largest membrane voltage 0.2202 to 0.2203 V, node voltages up to 2.85 to
  2.96 V, refused by the stiffness guard at step 1,312 to 1,465, where the
  old node bound refused them at step 316 to 377 (11.2).
- No verdict changed. Every other number is identical to the first receipt
  (11, history).
