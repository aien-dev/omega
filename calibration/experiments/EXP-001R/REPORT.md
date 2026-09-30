# EXP-001 compression bridge: report

EXP_001_COMPRESSION_BRIDGE = PASS

Profile sha256 0267bffc758147718e6ee95e40581d9456322dbc92f2cabdd91a01b9b86d74d3. Candidate manifest sha256 7edcd62e46c8af807b87c1a81dcc2aad14464ad9e54f936567ce52919725d307 (status frozen). Dataset EXP-001R-sealed-8e6c5acd9bc8e1f46fa57a878b130f0b9ccd5399 (sealed_test), payload acd574f386a38c14f7aa1d56a07b6474ec725994487dda4d880d46b07c777a7a.

Criteria: S1 PASS, S2 PASS, S3 PASS, S4 PASS, S5 PASS, S6 PASS, S7 PASS, S8 PASS, S9 PASS.

## Group 1 (primary): seeds 4189149351502956729 6417565952383260634 1904504941657097619, 553 crumbs, 6168907 events

All values in bits. T against B2; uncertainty = 95% crumb-bootstrap interval of T_ideal (T_A, T_B intervals are the same interval shifted).

| candidate | L(M) | ideal bits | coder-A bits | coder-B bits | T_ideal | T_A | T_B | uncertainty | result |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| B0_uniform | 232 | 19554701.191047 | 19559144 | 19552984 | -11663620.562244 | -11663916.000000 | -11660468.000000 | [-12476092.387581, -10854803.372435] | loses to B2 (reported) |
| B1_order0 | 232 | 10536675.381067 | 10540784 | 10538096 | -2645594.752264 | -2645556.000000 | -2645580.000000 | [-2827542.602014, -2464712.099628] | loses to B2 (reported) |
| B2_order1 | 1156 | 7890156.628803 | 7894304 | 7891592 | 0.000000 | 0.000000 | 0.000000 | [0.000000, 0.000000] | baseline |
| B3_heuristic | 2344 | 17317661.647723 | 17322472 | 17316792 | -9428693.018920 | -9429356.000000 | -9426388.000000 | [-10087834.924323, -8772769.052827] | loses to B2 (reported) |
| M_candidate | 180834 | 4996198.434215 | 5000832 | 4997632 | 2714280.194588 | 2713794.000000 | 2714282.000000 | [2516825.234290, 2914129.441491] | wins (S6 PASS rule) |
| M_mem | 607838516 | 9956358.549669 | 9960896 | 9956144 | -609903561.920866 | -609903952.000000 | -609901912.000000 | [-610176011.229713, -609629640.782651] | loses (S7 PASS rule) |
| M_mem_seed1 | 356335496 | 23493523.176382 | 23496288 | 23495048 | -371937706.547579 | -371936324.000000 | -371937796.000000 | [-373691369.329960, -370243728.034511] | loses (S7 PASS rule) |

Envelope (two-sided, per file and per crumb, |overhead - 448| <= 64 + 0.001 N bits):

- B0_uniform: overhead A 4442.808953 bits, B -1717.191047 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.858318, B 32.858318 bits
- B1_order0: overhead A 4108.618933 bits, B 1420.618933 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.352063, B 32.429908 bits
- B2_order1: overhead A 4147.371197 bits, B 1435.371197 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.224904, B 32.510146 bits
- B3_heuristic: overhead A 4810.352277 bits, B -869.647723 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.266248, B 32.489069 bits
- M_candidate: overhead A 4633.565785 bits, B 1433.565785 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.315952, B 32.517895 bits
- M_mem: overhead A 4537.450331 bits, B -214.549669 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.344983, B 32.522188 bits
- M_mem_seed1: overhead A 2764.823618 bits, B 1524.823618 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.440368, B 32.440368 bits

Spearman rho of the description-length order, ideal vs A 1.0000, ideal vs B 1.0000 (reported only).

## Group 2 (replication): seeds 3145886515248922810 1712640590493673198 2753470826557035650, 557 crumbs, 6199000 events

All values in bits. T against B2; uncertainty = 95% crumb-bootstrap interval of T_ideal (T_A, T_B intervals are the same interval shifted).

| candidate | L(M) | ideal bits | coder-A bits | coder-B bits | T_ideal | T_A | T_B | uncertainty | result |
|---|---:|---:|---:|---:|---:|---:|---:|---|---|
| B0_uniform | 232 | 19650092.421366 | 19654552 | 19648360 | -11723952.317058 | -11724204.000000 | -11720796.000000 | [-12527341.472072, -10918327.978165] | loses to B2 (reported) |
| B1_order0 | 232 | 10590628.415188 | 10594768 | 10592056 | -2664488.310880 | -2664420.000000 | -2664492.000000 | [-2843322.030863, -2484033.163855] | loses to B2 (reported) |
| B2_order1 | 1156 | 7925216.104308 | 7929424 | 7926640 | 0.000000 | 0.000000 | 0.000000 | [0.000000, 0.000000] | baseline |
| B3_heuristic | 2344 | 17402132.055621 | 17406960 | 17401248 | -9478103.951313 | -9478724.000000 | -9475796.000000 | [-10129805.462315, -8824952.620080] | loses to B2 (reported) |
| M_candidate | 180834 | 5019569.577267 | 5024184 | 5021000 | 2725968.527041 | 2725562.000000 | 2725962.000000 | [2529881.119038, 2920656.379409] | wins (S6 PASS rule) |
| M_mem | 607838516 | 9728924.119553 | 9733464 | 9728720 | -609641068.015245 | -609641400.000000 | -609639440.000000 | [-609915352.734391, -609375148.160124] | loses (S7 PASS rule) |
| M_mem_seed1 | 356335496 | 22600468.097945 | 22603296 | 22601984 | -371009591.993637 | -371008212.000000 | -371009684.000000 | [-372669338.402420, -369438769.372566] | loses (S7 PASS rule) |

Envelope (two-sided, per file and per crumb, |overhead - 448| <= 64 + 0.001 N bits):

- B0_uniform: overhead A 4459.578634 bits, B -1732.421366 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.389522, B 32.599931 bits
- B1_order0: overhead A 4139.584812 bits, B 1427.584812 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.538673, B 32.538673 bits
- B2_order1: overhead A 4207.895692 bits, B 1423.895692 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.330895, B 32.674497 bits
- B3_heuristic: overhead A 4827.944379 bits, B -884.055621 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.502477, B 32.595504 bits
- M_candidate: overhead A 4614.422733 bits, B 1430.422733 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.262249, B 32.533452 bits
- M_mem: overhead A 4539.880447 bits, B -204.119553 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.419061, B 32.506888 bits
- M_mem_seed1: overhead A 2827.902055 bits, B 1515.902055 bits over the group; units outside the band A 0, B 0; worst crumb margin A 32.444896, B 32.444896 bits

Spearman rho of the description-length order, ideal vs A 1.0000, ideal vs B 1.0000 (reported only).

Reversals: 0 found, 0 explained by the coder envelope (detail in uncertainty.json).

Sensitivity (T against B0, B1, B3; L(M) byte-rounded and doubled; data-only gain; interval inflated x2.04) is in uncertainty.json; it is reported, not part of the verdict.

## Report sections

- Preregistration: calibration/preregistration/EXP-001R.md and preregistration.json (sha256 in final_receipt.json freeze.preregistration_sha256).
- Blinding: calibration/docs/BLINDING_PROTOCOL.md; overlap audit PASS (overlap_audit.json).
- Candidate freeze: candidate_manifest.json sha256 7edcd62e46c8af807b87c1a81dcc2aad14464ad9e54f936567ce52919725d307, status frozen, frozen_at 2026-09-30T04:00:05Z, freeze commit C_f 8e6c5acd9bc8e1f46fa57a878b130f0b9ccd5399.
- Model-cost accounting: L(M) = exact TYM0 bits (calibration/docs/MODEL_DESCRIPTION_ENCODING.md), re-checked against the manifest for every candidate.
- Probability stream: one TPS1 per candidate and file, probability_root 2fe6f4bfb844cad9f68b58631ee2a596413f25598e70e87e375c6e08afc2000d (probability_streams/INDEX).
- Ideal codelength: ideal_lengths.json (per file and per crumb, int64 ub).
- Actual coder results: arithmetic/results.json, ans/results.json, encoded_artifacts/INDEX, decoder_receipts/; coding_root 67416eb09a583cff68877f623090c51b59b01a503ebfee61eb6728f1205de53f.
- Memorizer result: rows M_mem and M_mem_seed1 in the tables above (criterion S7, S9).
- Independent verification: S8 = PASS (scorer_independent.json vs scorer_primary.json).
- Uncertainty: uncertainty.json (calibration/docs/UNCERTAINTY_PROTOCOL.md).
- Failures/deviations: calibration/docs/PROTOCOL_CONFORMANCE.md (D1-D9).
- Gate decision: EXP_001_COMPRESSION_BRIDGE = PASS (rule: prereg section 5a).

S8 compared against an independent scorer file. S8 compared 312 values, 0 mismatches.

Verdict rule result: PASS.
